#ifndef APTOS_I18N_H
#define APTOS_I18N_H

#include "types.h"

#define I18N_MAX_LANGS 8
#define I18N_MAX_KEYS  128

typedef struct {
    const char *id;      /* "en" / "zh" / ... */
    const char *native;  /* "English" / "简体中文" */
} i18n_lang_t;

/* 注册表初始化（内置 en/zh 语言包），默认语言 en */
void         i18n_init(void);
/* 切换当前语言，返回 0 成功 / -1 无此语言 */
int          i18n_set(const char *id);
const char  *i18n_id(void);
const char  *i18n_native(void);
int          i18n_count(void);
const i18n_lang_t *i18n_lang_at(int i);
/* 翻译：查当前语言表，未命中返回 key 本身 */
const char  *i18n_tr(const char *key);

#define L(key) i18n_tr(key)

#endif
