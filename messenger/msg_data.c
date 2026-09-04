/* Модель данных мессенджера: чаты, сообщения, демо-данные первого запуска,
 * сохранение на диск и автоответчик с индикатором «печатает…». */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "msg.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

MsgChat msg_chats[MSG_MAX_CHATS];
int msg_chat_count = 0;
static char data_dir[512] = ".";
static int data_loaded = 0;

void msg_set_data_path(const char *dir) {
    if (dir && *dir) snprintf(data_dir, sizeof(data_dir), "%s", dir);
}

static double frand(double lo, double hi) {
    return lo + (hi - lo) * ((double)rand() / (double)RAND_MAX);
}

/* ── сообщения ─────────────────────────────────────────────────────────── */
static void sanitize(char *s) {
    for (char *p = s; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
}

void msg_chat_add(int ci, const char *text, int out, double t, int state) {
    if (ci < 0 || ci >= msg_chat_count || !text) return;
    MsgChat *c = &msg_chats[ci];
    if (c->count >= c->cap) {
        int nc = c->cap ? c->cap * 2 : 16;
        MsgMessage *nm = (MsgMessage *)realloc(c->msgs, (size_t)nc * sizeof(*nm));
        if (!nm) return;
        c->msgs = nm;
        c->cap = nc;
    }
    MsgMessage *m = &c->msgs[c->count++];
    memset(m, 0, sizeof(*m));
    snprintf(m->text, sizeof(m->text), "%s", text);
    sanitize(m->text);
    m->t = t;
    m->out = out;
    m->state = state;
    c->gen++;
}

/* ── автоответчик ──────────────────────────────────────────────────────── */
static const char *GENERIC_REPLIES[] = {
    "Понял, спасибо!",
    "Ага, сейчас гляну.",
    "Звучит отлично.",
    "Хорошо, договорились.",
    "Ого, интересно! Расскажешь подробнее?",
    "Да, я тоже об этом думал.",
    "Супер, продолжаем в том же духе.",
    "Ок!",
    "Секунду, отвечу развёрнуто чуть позже.",
    "Класс! Именно это и хотел услышать.",
};
static const char *ANDREY_REPLIES[] = {
    "Кстати, интерфейс реально стал как в Телеграме. Пузыри, галочки — всё по красоте.",
    "А тёмную тему когда добавишь?",
    "Скинь потом сборку, покажу нашим.",
    "Шрифт круглый — прям как раньше, только теперь всё на чистом C?",
    "Понял. Тогда жду апк!",
};
static const char *TEAM_REPLIES[] = {
    "Максим: Принято, внесу в план.",
    "Ася: Отлично, тогда фиксируем.",
    "Максим: Звуки, кстати, уже в сборке.",
    "Ася: Напоминаю про созвон в 12:00.",
};
static const char *MARIA_REPLIES[] = {
    "Ахах, ну ты и хитрый)",
    "Ладно, уговорил. Тогда вечером списываемся.",
    "Смотри не заспой мне концовку!",
    "Кстати, там новая серия вышла, глянь.",
};
static const char *DIMA_REPLIES[] = {
    "Ну так что, го в настолки?",
    "Я уже и пиццу заказал))",
    "Не молчи!",
    "Ау!",
};
static const char *BOT_REPLIES[] = {
    "Я бот-помощник: получил ваше сообщение и сразу отвечаю. Так работает автоответчик на C — без всякого скриптового движка.",
    "Принято! Если хочется проверить длинные пузыри — напишите что-нибудь подлиннее, переносы строк считаются автоматически.",
    "Отвечаю с небольшой задержкой и индикатором «печатает», как в настоящих мессенджерах.",
    "Сообщение доставлено: галочки вверху пузыря уже зелёные.",
};

static const char *pick_reply(int ci) {
    MsgChat *c = &msg_chats[ci];
    const char **pool = GENERIC_REPLIES;
    int n = (int)(sizeof(GENERIC_REPLIES) / sizeof(pool[0]));
    if (ci == 0) { pool = ANDREY_REPLIES; n = (int)(sizeof(ANDREY_REPLIES)/sizeof(pool[0])); }
    else if (ci == 1) { pool = TEAM_REPLIES; n = (int)(sizeof(TEAM_REPLIES)/sizeof(pool[0])); }
    else if (ci == 2) { pool = MARIA_REPLIES; n = (int)(sizeof(MARIA_REPLIES)/sizeof(pool[0])); }
    else if (ci == 3) { pool = DIMA_REPLIES; n = (int)(sizeof(DIMA_REPLIES)/sizeof(pool[0])); }
    else if (strcmp(c->name, "Бот-помощник") == 0) { pool = BOT_REPLIES; n = (int)(sizeof(BOT_REPLIES)/sizeof(pool[0])); }
    int pick = rand() % n;
    if (pick == c->reply_pick && n > 1) pick = (pick + 1) % n;
    c->reply_pick = pick;
    return pool[pick];
}

void msg_send(int ci, const char *text) {
    if (ci < 0 || ci >= msg_chat_count || !text || !*text) return;
    MsgChat *c = &msg_chats[ci];
    msg_chat_add(ci, text, 1, msg_wall(), c->saved ? 2 : 0);
    snd_play("send.wav");
    /* «Избранное» и каналы не отвечают; остальным включаем автоответчик. */
    if (!c->saved && strcmp(c->name, "Новости Enjoer") != 0) {
        c->reply_phase = 1;
        c->reply_t1 = msg_now() + frand(0.7, 1.6);
        c->reply_t2 = c->reply_t1 + frand(1.1, 2.4);
    }
    msg_data_save();
}

void msg_data_update(void) {
    double now = msg_now(), wall = msg_wall();
    for (int i = 0; i < msg_chat_count; i++) {
        MsgChat *c = &msg_chats[i];
        /* часики → одна галочка через полсекунды */
        if (c->count > 0) {
            MsgMessage *last = &c->msgs[c->count - 1];
            if (last->out && last->state == 0 && wall - last->t > 0.5) {
                last->state = 1;
                c->gen++;
                msg_data_save();
            }
        }
        if (c->reply_phase == 1 && now >= c->reply_t1) {
            c->reply_phase = 2;
            c->online = 1;
            c->gen++;
        } else if (c->reply_phase == 2 && now >= c->reply_t2) {
            const char *reply = pick_reply(i);
            msg_chat_add(i, reply, 0, wall, 0);
            c->reply_phase = 0;
            /* мои сообщения в этом чате теперь прочитаны */
            for (int k = 0; k < c->count; k++)
                if (c->msgs[k].out && c->msgs[k].state < 2) c->msgs[k].state = 2;
            c->gen++;
            if (chat_is_open() && chat_current() == i) {
                /* чат открыт — непрочитанных нет, просто мягкий звук */
                snd_play("notify.wav");
            } else {
                c->unread++;
                snd_play("notify.wav");
            }
            msg_data_save();
        }
    }
}

const char *msg_last_seen(int ci) {
    if (ci < 0 || ci >= msg_chat_count) return "";
    MsgChat *c = &msg_chats[ci];
    if (c->saved) return "ваши заметки";
    if (c->reply_phase == 2) return "печатает…";
    if (strcmp(c->name, "Новости Enjoer") == 0) return "2 481 подписчик";
    if (c->group) return "6 участников";
    if (c->online) return "в сети";
    return "был(а) недавно";
}

/* ── сохранение и загрузка ─────────────────────────────────────────────── */
static void data_path(char *out, size_t n) {
    snprintf(out, n, "%s/enjoer_chats.dat", data_dir);
}

void msg_data_save(void) {
    char path[600];
    data_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "ENJOER1\n");
    for (int i = 0; i < msg_chat_count; i++) {
        MsgChat *c = &msg_chats[i];
        fprintf(f, "C|%s|%d|%d|%d|%d|%d|%d\n", c->name, c->color, c->online,
                c->saved, c->group, c->unread, c->muted);
    }
    for (int i = 0; i < msg_chat_count; i++) {
        MsgChat *c = &msg_chats[i];
        for (int k = 0; k < c->count; k++) {
            MsgMessage *m = &c->msgs[k];
            fprintf(f, "M|%d|%d|%.0f|%d|%s\n", i, m->out, m->t, m->state, m->text);
        }
    }
    fclose(f);
}

static int msg_data_load(void) {
    char path[600], line[2048];
    data_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = '\0';
        if (!L) continue;
        if (!ok) {
            if (strcmp(line, "ENJOER1") != 0) break;
            ok = 1;
            continue;
        }
        if (line[0] == 'C' && line[1] == '|') {
            if (msg_chat_count >= MSG_MAX_CHATS) continue;
            MsgChat *c = &msg_chats[msg_chat_count];
            memset(c, 0, sizeof(*c));
            char name[128] = {0};
            if (sscanf(line + 2, "%127[^|]|%d|%d|%d|%d|%d|%d", name, &c->color,
                       &c->online, &c->saved, &c->group, &c->unread, &c->muted) == 7) {
                snprintf(c->name, sizeof(c->name), "%s", name);
                msg_chat_count++;
            }
        } else if (line[0] == 'M' && line[1] == '|') {
            int ci = 0, out = 0, state = 0;
            double t = 0;
            char *p = line + 2;
            char *e1 = strchr(p, '|'); if (!e1) continue; *e1 = 0; ci = atoi(p); p = e1 + 1;
            char *e2 = strchr(p, '|'); if (!e2) continue; *e2 = 0; out = atoi(p); p = e2 + 1;
            char *e3 = strchr(p, '|'); if (!e3) continue; *e3 = 0; t = atof(p); p = e3 + 1;
            char *e4 = strchr(p, '|'); if (!e4) continue; *e4 = 0; state = atoi(p); p = e4 + 1;
            if (ci >= 0 && ci < msg_chat_count && *p)
                msg_chat_add(ci, p, out, t, state);
        }
    }
    fclose(f);
    return ok && msg_chat_count > 0;
}

/* ── демо-данные первого запуска ───────────────────────────────────────── */
static void seed_chat(const char *name, int color, int online, int saved, int group) {
    if (msg_chat_count >= MSG_MAX_CHATS) return;
    MsgChat *c = &msg_chats[msg_chat_count++];
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->color = color;
    c->online = online;
    c->saved = saved;
    c->group = group;
}

static void msg_data_seed(void) {
    double now = msg_wall();
    srand((unsigned int)now);

    seed_chat("Андрей Смирнов", 0, 1, 0, 0);
    msg_chat_add(0, "Привет! Видел новую сборку мессенджера?", 0, now - 26*3600, 0);
    msg_chat_add(0, "Привет! Да, интерфейс уже почти как в Телеграме", 1, now - 25.8*3600, 2);
    msg_chat_add(0, "Шрифт тот же оставили? Круглый такой, жирный?", 0, now - 25.6*3600, 0);
    msg_chat_add(0, "Да, Chill Round Gothic, Heavy. Рендер свой, всё написано на чистом C", 1, now - 25.5*3600, 2);
    msg_chat_add(0, "Красиво! Скинь потом сборку", 0, now - 31*60, 0);
    msg_chat_add(0, "И глянь список чатов — анимации стали плавнее", 0, now - 30*60, 0);
    msg_chats[0].unread = 2;

    seed_chat("Команда Enjoer", 2, 0, 0, 1);
    msg_chat_add(1, "Ася: Релиз в пятницу, все помнят?", 0, now - 3.0*3600, 0);
    msg_chat_add(1, "Максим: Залил новые звуки отправки и уведомлений", 0, now - 2.7*3600, 0);
    msg_chat_add(1, "Проверил, звучит отлично", 1, now - 2.5*3600, 2);
    msg_chat_add(1, "Ася: Тогда собираем билд завтра в 12:00", 0, now - 42*60, 0);
    msg_chats[1].unread = 1;

    seed_chat("Мария", 7, 0, 0, 0);
    msg_chat_add(2, "Кстати, ты досмотрел тот сериал?", 0, now - 26*3600, 0);
    msg_chat_add(2, "Ещё нет, остановился на пятой серии", 1, now - 25.9*3600, 2);
    msg_chat_add(2, "Без спойлеров! Там дальше неожиданный поворот", 0, now - 25.8*3600, 0);
    msg_chat_add(2, "Ладно, молчу)", 1, now - 25.7*3600, 2);

    seed_chat("Дима Карпов", 1, 1, 0, 0);
    msg_chat_add(3, "Го в настолки в субботу?", 0, now - 55*60, 0);
    msg_chat_add(3, "Я собрал старую компанию, будет весело", 0, now - 54*60, 0);
    msg_chat_add(3, "Ну что, ты в деле?", 0, now - 12*60, 0);
    msg_chats[3].unread = 12;
    msg_chats[3].muted = 1;

    seed_chat("Избранное", 4, 0, 1, 0);
    msg_chat_add(4, "Список дел: доделать рендер треугольников, собрать шрифты в атлас", 1, now - 2*86400, 2);
    msg_chat_add(4, "Идея: тёмная тема для мессенджера", 1, now - 26*3600, 2);

    seed_chat("Новости Enjoer", 5, 0, 0, 0);
    msg_chat_add(5, "Вышло обновление 1.2: ускоренный рендер текста и новые аватарки", 0, now - 5*3600, 0);
    msg_chat_add(5, "Скоро: пересылка сообщений и реакции", 0, now - 1*3600, 0);
    msg_chats[5].unread = 1;

    seed_chat("Бот-помощник", 3, 1, 0, 0);
    msg_chat_add(6, "Привет! Напишите что-нибудь — я отвечу. Так работает демо-автоответчик.", 0, now - 20*3600, 0);
}

void msg_data_init(void) {
    if (data_loaded) return;
    data_loaded = 1;
    mkdir(data_dir, 0755);
    if (!msg_data_load()) {
        msg_data_seed();
        msg_data_save();
    }
}
