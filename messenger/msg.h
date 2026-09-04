/* Enjoer Messenger — интерфейс в духе Telegram, написанный на чистом C
 * поверх того же рендера и шрифта, что использовал DimScript
 * (graphics.c + ttf_font.c + ChillRoundGothic_Heavy.ttf).
 *
 * Это общий заголовок: модель данных, утилиты интерфейса и экраны. */
#ifndef MSG_H
#define MSG_H

#include "runtime.h"

/* ── Палитра (0xAARRGGBB; у рендера альфа 00 означает «полностью непрозрачный») ── */
#define C_WHITE       0xFFFFFFFFu
#define C_BLACK       0xFF000000u
#define C_ACCENT      0xFF2AABEEu  /* фирменный синий Telegram */
#define C_ACCENT_DK   0xFF1F97D6u
#define C_TEXT_MAIN   0xFF0F0F0Fu
#define C_TEXT_SEC    0xFF707579u  /* превью сообщения, подписи */
#define C_TEXT_HINT   0xFF95999Cu  /* время, плейсхолдеры */
#define C_DIVIDER     0xFFE9E9E9u
#define C_SEARCH_BG   0xFFF1F1F1u
#define C_GREEN       0xFF43B54Cu  /* бейджи непрочитанных */
#define C_ONLINE      0xFF3FBF55u
#define C_BUBBLE_IN   0xFFFFFFFFu
#define C_BUBBLE_OUT  0xFFEFFDDEu  /* классический светло-зелёный исходящих */
#define C_TIME_IN     0xFFA0ACB4u
#define C_TIME_OUT    0xFF57A856u
#define C_DATE_PILL   0xB3FFFFFFu  /* полупрозрачный белый */
#define C_WALL_TOP    0xFFD6E6CEu  /* обои чата: мягкий зелёный градиент */
#define C_WALL_BOT    0xFFA9CDA1u
#define C_PRESS       0x14000000u  /* подсветка нажатия */

#define MSG_MAX_CHATS 32
#define MSG_TEXT_MAX  900
#define MSG_NAME_MAX  64
#define MSG_DRAFT_MAX 360

typedef struct {
    char text[MSG_TEXT_MAX];
    double t;      /* unix-время */
    int out;       /* 1 — исходящее */
    int state;     /* исходящие: 0 отправляется (часики), 1 отправлено ✓, 2 прочитано ✓✓ */
} MsgMessage;

typedef struct {
    char name[MSG_NAME_MAX];
    int color;     /* индекс палитры аватарок */
    int online;
    int saved;     /* «Избранное» */
    int group;     /* групповой чат: в префиксе видно автора */
    int unread;
    int muted;
    MsgMessage *msgs;
    int count, cap;
    long gen;      /* растёт при изменениях — инвалидация кэша раскладки */
    /* автоответчик: 0 — тихо, 1 — ждём начала «печатает», 2 — печатает */
    int reply_phase;
    double reply_t1;   /* когда начать печатать */
    double reply_t2;   /* когда придёт ответ */
    int reply_pick;
} MsgChat;

extern MsgChat msg_chats[MSG_MAX_CHATS];
extern int msg_chat_count;
double msg_now(void);   /* монотонные секунды — для анимаций и таймеров */
double msg_wall(void);  /* unix-время — для меток сообщений */

/* msg_data.c — модель, демо-данные, сохранение, автоответы */
void msg_set_data_path(const char *dir);
void msg_data_init(void);
void msg_data_save(void);
void msg_data_update(void);
void msg_chat_add(int ci, const char *text, int out, double t, int state);
void msg_send(int ci, const char *text);
const char *msg_last_seen(int ci);         /* «в сети» / «был(а) недавно» */

/* msg_ui.c — примитивы интерфейса */
float msg_u(void);
void msg_avatar(float cx, float cy, float r, int color, const char *name, int saved);
void msg_poly(const float *xy, int n, uint32_t c);
void msg_arc(float cx, float cy, float r, float a0, float a1, float th, uint32_t c);
void ico_menu(float cx, float cy, float s, uint32_t c);
void ico_search(float cx, float cy, float s, uint32_t c);
void ico_back(float cx, float cy, float s, uint32_t c);
void ico_pencil(float cx, float cy, float s, uint32_t c);
void ico_send(float cx, float cy, float s, uint32_t c);
void ico_mic(float cx, float cy, float s, uint32_t c);
void ico_smile(float cx, float cy, float s, uint32_t c);
void ico_clip(float cx, float cy, float s, uint32_t c);
void ico_check(float cx, float cy, float s, uint32_t c);
void ico_checks(float cx, float cy, float s, uint32_t c);
void ico_clock(float cx, float cy, float s, uint32_t c);
void ico_close(float cx, float cy, float s, uint32_t c);
void ico_mute(float cx, float cy, float s, uint32_t c);
int msg_text_w(const char *s, float scale);
float msg_line_h(float scale);
void msg_draw_text_c(const char *s, float cx, float cy, float scale, uint32_t c);
void msg_text_fit(const char *s, float scale, int maxw, char *out, int outsz);
void msg_fmt_time(double t, char *out, int n);   /* список чатов */
void msg_fmt_hm(double t, char *out, int n);     /* 14:32 */
void msg_fmt_day(double t, char *out, int n);    /* Сегодня / Вчера / 5 марта */
int msg_same_day(double a, double b);

/* msg_chats.c — экран «список чатов» */
void chats_reset(void);
void chats_update(void);
void chats_draw(float dx);
int chats_touch(float x, float y, int action);

/* msg_chat.c — экран разговора */
void chat_open(int ci);
void chat_close_request(void);
int chat_wants_close(void);
void chat_finish_close(void);
int chat_is_open(void);
int chat_current(void);
void chat_reset(void);
void chat_update(void);
void chat_draw(float dx);
int chat_touch(float x, float y, int action);

#endif
