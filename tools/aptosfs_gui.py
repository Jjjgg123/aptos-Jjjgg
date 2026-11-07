#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
aptosfs_gui.py — miniFS 镜像文件浏览器（tkinter，Python 自带，无需额外依赖）

用法：
  python aptosfs_gui.py
  python aptosfs_gui.py G:\\aptos-os\\aptos-os406.img

功能：浏览目录、看文件内容、从宿主导入、导出到宿主、新建目录、删除。
写操作会直接改镜像，务必先关闭虚拟机。
"""

import os
import sys
import tkinter as tk
from tkinter import ttk, filedialog, messagebox, simpledialog

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aptosfs import MiniFS, FsError


def fmt_size(n, is_dir=False):
    if is_dir:
        return "<DIR>"
    if n < 1024:
        return "%d B" % n
    if n < 1024 * 1024:
        return "%.1f KB" % (n / 1024.0)
    return "%.1f MB" % (n / 1048576.0)


class App(object):
    def __init__(self, root, image=None):
        self.root = root
        self.fs = None
        self.cwd = "/"
        root.title("aptos miniFS 浏览器")
        root.geometry("880x600")
        root.minsize(720, 480)

        style = ttk.Style()
        try:
            style.theme_use("clam")
        except Exception:
            pass

        # 顶部工具栏
        top = ttk.Frame(root, padding=(8, 8, 8, 4))
        top.pack(fill="x")

        ttk.Button(top, text="打开镜像", command=self.open_image).pack(side="left")
        ttk.Button(top, text="刷新", command=self.refresh).pack(side="left", padx=(6, 0))
        ttk.Button(top, text="上级", command=self.go_up).pack(side="left", padx=(6, 0))

        self.path_var = tk.StringVar(value="/")
        e = ttk.Entry(top, textvariable=self.path_var)
        e.pack(side="left", fill="x", expand=True, padx=(10, 6))
        e.bind("<Return>", lambda ev: self.goto(self.path_var.get()))

        ttk.Button(top, text="转到", command=lambda: self.goto(self.path_var.get())).pack(side="left")

        # 文件列表
        mid = ttk.Frame(root, padding=(8, 0))
        mid.pack(fill="both", expand=True)

        cols = ("name", "size", "block", "date")
        self.tree = ttk.Treeview(mid, columns=cols, show="headings", selectmode="browse")
        self.tree.heading("name", text="名称")
        self.tree.heading("size", text="大小")
        self.tree.heading("block", text="起始块")
        self.tree.heading("date", text="修改日期")
        self.tree.column("name", width=380, anchor="w")
        self.tree.column("size", width=100, anchor="e")
        self.tree.column("block", width=100, anchor="e")
        self.tree.column("date", width=110, anchor="center")

        sb = ttk.Scrollbar(mid, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=sb.set)
        self.tree.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")

        self.tree.bind("<Double-1>", self.on_open)
        self.tree.bind("<Return>", self.on_open)

        # 底部按钮
        bot = ttk.Frame(root, padding=(8, 6, 8, 8))
        bot.pack(fill="x")

        ttk.Button(bot, text="导入文件 → 镜像", command=self.put_file).pack(side="left")
        ttk.Button(bot, text="导出镜像文件 → 本地", command=self.get_file).pack(side="left", padx=(6, 0))
        ttk.Button(bot, text="新建目录", command=self.mkdir).pack(side="left", padx=(6, 0))
        ttk.Button(bot, text="删除", command=self.delete).pack(side="left", padx=(6, 0))
        ttk.Button(bot, text="查看内容", command=self.view).pack(side="left", padx=(6, 0))

        self.status = tk.StringVar(value="未打开镜像")
        ttk.Label(root, textvariable=self.status, anchor="w", padding=(10, 4)).pack(fill="x")

        if image:
            self.load(image)

    # ---------- 镜像 ----------

    def open_image(self):
        p = filedialog.askopenfilename(
            title="选择 aptos 镜像",
            filetypes=[("磁盘镜像", "*.img *.raw"), ("所有文件", "*.*")])
        if p:
            self.load(p)

    def load(self, path):
        if self.fs:
            self.fs.close()
            self.fs = None
        try:
            self.fs = MiniFS(path, writable=True)
        except FsError as ex:
            messagebox.showerror("打开失败", str(ex))
            return
        except PermissionError:
            messagebox.showerror("打开失败",
                                 "镜像被占用。\n如果虚拟机正在运行，请先完全关闭。")
            return
        self.cwd = "/"
        self.refresh()

    def refresh(self):
        if not self.fs:
            self.status.set("未打开镜像")
            return
        self.tree.delete(*self.tree.get_children())
        try:
            items = self.fs.listdir(self.cwd)
        except FsError as ex:
            messagebox.showerror("读取目录失败", str(ex))
            return
        rows = []
        for _, e in items:
            rows.append(("%s%s" % (e.name, "/" if e.is_dir else ""), e))
        for label, e in rows:
            self.tree.insert("", "end", text=label,
                             values=(label, fmt_size(e.size, e.is_dir), e.first, e.mdate))
            self.tree.set(self.tree.get_children()[-1], "name", label)
        self.path_var.set(self.cwd)
        info = self.fs.info()
        self.status.set("%s    FS@LBA %d    %s 已用 %.1f%%    %d 块空闲 / 共 %d 块"
                        % (os.path.basename(self.fs.path), info["fs_lba"], info["label"],
                           info["usage_pct"], info["free_blocks"], info["total_blocks"]))

    def selected_name(self):
        sel = self.tree.selection()
        if not sel:
            return None
        label = self.tree.item(sel[0], "text")
        return label.rstrip("/")

    def join(self, name):
        base = self.cwd.rstrip("/")
        return "%s/%s" % (base, name)

    def goto(self, path):
        path = path if path.startswith("/") else "/" + path
        self.cwd = path
        self.refresh()

    def go_up(self):
        if self.cwd == "/":
            return
        parts = [p for p in self.cwd.split("/") if p]
        self.goto("/" + "/".join(parts[:-1]) if len(parts) > 1 else "/")

    def on_open(self, ev=None):
        name = self.selected_name()
        if not name:
            return
        full = self.join(name)
        try:
            st, en, idx, e = self.fs.resolve(full)
        except FsError:
            return
        if e is None:
            return
        if e.is_dir:
            self.goto(full)
        else:
            self.show_content(full)

    # ---------- 操作 ----------

    def put_file(self):
        if not self.fs:
            return
        p = filedialog.askopenfilename(title="选择要写入镜像的文件")
        if not p:
            return
        name = simpledialog.askstring("目标文件名", "写入到 %s 下，文件名：" % self.cwd,
                                      initialvalue=os.path.basename(p).upper())
        if not name:
            return
        dest = self.join(name)
        try:
            with open(p, "rb") as f:
                data = f.read()
            self.fs.write_file(dest, data)
        except (FsError, OSError) as ex:
            messagebox.showerror("写入失败", str(ex))
            return
        self.refresh()
        self.status.set("已写入 %s（%s）" % (dest, fmt_size(len(data))))

    def get_file(self):
        if not self.fs:
            return
        name = self.selected_name()
        if not name:
            messagebox.showinfo("提示", "先选中一个文件")
            return
        full = self.join(name)
        try:
            data = self.fs.read_file(full)
        except FsError as ex:
            messagebox.showerror("读取失败", str(ex))
            return
        out = filedialog.asksaveasfilename(title="导出到", initialfile=name)
        if not out:
            return
        with open(out, "wb") as f:
            f.write(data)
        self.status.set("已导出 %s（%s）" % (out, fmt_size(len(data))))

    def mkdir(self):
        if not self.fs:
            return
        name = simpledialog.askstring("新建目录", "目录名：")
        if not name:
            return
        try:
            self.fs.mkdir(self.join(name))
        except FsError as ex:
            messagebox.showerror("创建失败", str(ex))
            return
        self.refresh()

    def delete(self):
        if not self.fs:
            return
        name = self.selected_name()
        if not name:
            messagebox.showinfo("提示", "先选中一项")
            return
        if not messagebox.askyesno("确认", "删除 %s ？" % name):
            return
        try:
            self.fs.delete(self.join(name))
        except FsError as ex:
            messagebox.showerror("删除失败", str(ex))
            return
        self.refresh()

    def view(self):
        if not self.fs:
            return
        name = self.selected_name()
        if not name:
            messagebox.showinfo("提示", "先选中一个文件")
            return
        self.show_content(self.join(name))

    def show_content(self, path):
        try:
            data = self.fs.read_file(path)
        except FsError as ex:
            messagebox.showerror("读取失败", str(ex))
            return
        win = tk.Toplevel(self.root)
        win.title("内容预览 — %s" % path)
        win.geometry("760x520")
        txt = tk.Text(win, wrap="none", font=("Consolas", 10))
        sbv = ttk.Scrollbar(win, orient="vertical", command=txt.yview)
        sbh = ttk.Scrollbar(win, orient="horizontal", command=txt.xview)
        txt.configure(yscrollcommand=sbv.set, xscrollcommand=sbh.set)
        txt.grid(row=0, column=0, sticky="nsew")
        sbv.grid(row=0, column=1, sticky="ns")
        sbh.grid(row=1, column=0, sticky="ew")
        win.grid_rowconfigure(0, weight=1)
        win.grid_columnconfigure(0, weight=1)
        body = data.decode("utf-8", "replace")
        txt.insert("1.0", body)
        txt.configure(state="disabled")
        win.title("内容预览 — %s（%s）" % (path, fmt_size(len(data))))

    def on_close(self):
        if self.fs:
            self.fs.close()
        self.root.destroy()


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else None
    root = tk.Tk()
    app = App(root, image)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()


if __name__ == "__main__":
    main()
