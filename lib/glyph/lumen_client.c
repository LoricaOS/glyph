/* lumen_client.c — Lumen external window protocol client implementation */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>

#include "lumen_client.h"

/* Write a diagnostic line to BOTH stderr AND /dev/console.
 * stderr may be a PTY (invisible in test-harness serial output) when
 * the client was launched from a Lumen terminal window. /dev/console
 * always reaches the kernel serial driver. */
static void
lumen_diag(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    write(2, buf, (size_t)n);
    int cfd = open("/dev/console", O_WRONLY);
    if (cfd >= 0) { write(cfd, buf, (size_t)n); close(cfd); }
}

/* Read exactly n bytes from a stream fd, accumulating across short reads.
 * AF_UNIX SOCK_STREAM gives no message-boundary guarantee: a single read() of
 * a fixed-size protocol struct can return fewer bytes than requested, which
 * desyncs the framed wire format (every subsequent header then lands
 * mid-struct). This is the framing/reassembly primitive for Lumen clients —
 * read the header, then read exactly the body. Retries EINTR and EAGAIN (the
 * caller polls for readability first, and a partially-delivered frame's
 * remaining bytes are imminent on a local socket). Returns 0 on success, -1 on
 * peer close (EOF) or a hard error. Exported for other external clients. */
int lumen_read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r > 0) { got += (size_t)r; continue; }
        if (r == 0) return -1;                 /* peer closed mid-frame */
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
        return -1;                             /* hard error */
    }
    return 0;
}

/* Write exactly n bytes, looping over short writes. AF_UNIX SOCK_STREAM can
 * accept fewer bytes than requested for a large message (e.g. SET_MENU), so a
 * single write() would truncate the frame and desync the peer. */
int lumen_write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, p + sent, n - sent);
        if (w > 0) { sent += (size_t)w; continue; }
        if (w == 0) return -1;
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
        return -1;
    }
    return 0;
}

int lumen_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        lumen_diag( "[LUMEN-CLI] socket: errno=%d\n", errno);
        return -errno;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/run/lumen.sock",
            sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int err = errno;
        /* Silent on ECONNREFUSED (server not yet up — caller will retry).
         * Surface anything else immediately. */
        if (err != 111)
            lumen_diag( "[LUMEN-CLI] connect: errno=%d\n", err);
        close(fd);
        return -err;
    }

    lumen_hello_t hello = { LUMEN_MAGIC, LUMEN_VERSION };
    ssize_t wn = write(fd, &hello, sizeof(hello));
    if (wn != (ssize_t)sizeof(hello)) {
        lumen_diag( "[LUMEN-CLI] write hello: rc=%ld errno=%d\n", (long)wn, errno);
        close(fd); return -EIO;
    }

    lumen_hello_reply_t reply;
    ssize_t rn = read(fd, &reply, sizeof(reply));
    if (rn != (ssize_t)sizeof(reply)) {
        lumen_diag( "[LUMEN-CLI] read reply: rc=%ld errno=%d\n", (long)rn, errno);
        close(fd); return -EIO;
    }
    if (reply.magic != LUMEN_MAGIC || reply.status != 0) {
        lumen_diag( "[LUMEN-CLI] bad reply magic=0x%x status=%u\n",
                reply.magic, reply.status);
        close(fd); return -EPROTO;
    }

    return fd;
}

/* Connect, retrying while the compositor's listener isn't up yet. Aegis
 * AF_UNIX returns ECONNREFUSED (-111) until lumen's accept() runs, so a
 * client spawned alongside the compositor races its startup. 50×100ms = 5s.
 * Any non-ECONNREFUSED result (success or hard error) returns immediately. */
int lumen_connect_retry(void)
{
    int fd = -111;
    for (int attempt = 0; attempt < 50; attempt++) {
        fd = lumen_connect();
        if (fd != -111) break;
        usleep(100000);
    }
    return fd;
}

/* ── Interleaved-event handling ───────────────────────────────────────────
 * The server can push an async event (e.g. LUMEN_EV_MENU_STATE, sent to a
 * shell panel on every focus change) on this same connection at ANY time —
 * including while we're mid-synchronous-reply-wait for CREATE_WINDOW/
 * CREATE_PANEL/RESIZE_SELF/RESIZE_BUFFER. Those reply-readers assume "the
 * next message is the reply" (hdr.op==0); an event arriving first would be
 * misread as reply data (and, since events carry no SCM_RIGHTS, "no memfd").
 *
 * MSG_PEEK is NOT used here — Aegis's socket implementation doesn't honor it
 * (verified: it silently consumes the peeked bytes anyway, desyncing the
 * stream). So drain_interleaved_events() genuinely consumes each header via
 * the ordinary lumen_read_full, decodes+stashes it if it's an event (at most
 * one — see the note below), and only returns once it has consumed a REAL
 * reply header (op==0) — which the caller must NOT re-read; it goes on to
 * recvmsg() just the body+cmsg (a single iovec), not the header again.
 * lumen_poll_event/lumen_wait_event check the stash before touching the
 * socket, so a stashed event is never lost, just reordered ahead of
 * whatever the caller was already waiting to send/receive. */
/* Ring of events consumed while waiting for a synchronous reply. This was a
 * SINGLE slot, on the reasoning that a reply-wait only ever races one push.
 * That stopped being true once the desktop shell became a client: it calls
 * lumen_window_resize_self() from inside its own mouse handling (opening a
 * dropdown resizes the panel), and the pointer keeps moving over the bar for
 * the whole round-trip — so several MOUSE events land during one wait. With
 * one slot, all but the last were dropped; losing a button press (or the
 * release that matches an earlier press) leaves the bar's state machine stuck
 * and it stops responding to clicks until something else resyncs it.
 *
 * Snapshot events (MENU_STATE, WINDOW_LIST) keep their payload in a single
 * static below, so queued copies of those all point at the newest snapshot —
 * fine, since each is a complete state, not a delta. */
#define EV_QUEUE_MAX 32
static lumen_event_t s_ev_queue[EV_QUEUE_MAX];
static int s_ev_head, s_ev_tail;

static int ev_queue_empty(void) { return s_ev_head == s_ev_tail; }

static void ev_queue_push(const lumen_event_t *ev)
{
    int next = (s_ev_tail + 1) % EV_QUEUE_MAX;
    if (next == s_ev_head) {
        /* Full: drop the OLDEST. Newer input is the more useful thing to
         * keep, and a full queue means the caller is badly behind anyway. */
        s_ev_head = (s_ev_head + 1) % EV_QUEUE_MAX;
    }
    s_ev_queue[s_ev_tail] = *ev;
    s_ev_tail = next;
}

static int ev_queue_pop(lumen_event_t *out)
{
    if (ev_queue_empty()) return 0;
    *out = s_ev_queue[s_ev_head];
    s_ev_head = (s_ev_head + 1) % EV_QUEUE_MAX;
    return 1;
}

static int recv_event_body(int fd, const lumen_msg_hdr_t *hdr, lumen_event_t *ev);

/* Consumes headers (and, for events, their bodies) until it consumes one
 * with op==0 — the reply the caller is waiting for. That header is NOT
 * re-readable; the caller must recvmsg() only the body+cmsg next. */
static int drain_interleaved_events(int fd)
{
    for (;;) {
        lumen_msg_hdr_t hdr;
        if (lumen_read_full(fd, &hdr, sizeof(hdr)) != 0) return -1;
        if (hdr.op == 0) return 0;
        lumen_event_t ev;
        if (recv_event_body(fd, &hdr, &ev) != 1) return -1;
        /* At most one slot: in practice a reply-wait only ever races a
         * single push (the caller drains the stash on its very next
         * lumen_poll_event/lumen_wait_event call before issuing another
         * request), so an overwrite here would mean two events interleaved
         * before either was drained — not reachable in this client's usage
         * pattern today. */
        ev_queue_push(&ev);
    }
}

static int recv_event(int fd, lumen_event_t *ev)
{
    lumen_msg_hdr_t hdr;
    if (lumen_read_full(fd, &hdr, sizeof(hdr)) != 0) return -1;
    /* op==0 is a REPLY. Reaching the event path means either a reply nobody
     * asked for or, far more likely, a desynced stream (a truncated frame
     * leaves us reading a body as headers — mostly zeros, so op=0/len=0,
     * which consumes nothing and spins forever with the UI frozen). Report it
     * as a lost connection instead: the caller exits and can be restarted,
     * which is recoverable, whereas a silent 100%-CPU spin is not. */
    if (hdr.op == 0) {
        /* A reply where an event was expected: either a stray reply nobody is
         * waiting for, or we are mid-body of a frame and reading its bytes as
         * headers. Drain by length and carry on — that resyncs the stray-reply
         * case. A zero length consumes nothing, so cap consecutive ones rather
         * than spinning at 100% CPU with the UI frozen (the top-bar hang). */
        static int zero_run;
        if (hdr.len == 0) {
            if (++zero_run > 64) return -1;
            return 0;
        }
        zero_run = 0;
        char tmp[256];
        uint32_t rem = hdr.len;
        while (rem > 0) {
            ssize_t r = read(fd, tmp, rem < sizeof(tmp) ? rem : sizeof(tmp));
            if (r <= 0) return -1;
            rem -= (uint32_t)r;
        }
        return 0;
    }
    return recv_event_body(fd, &hdr, ev);
}

static int recv_event_body(int fd, const lumen_msg_hdr_t *hdrp, lumen_event_t *ev)
{
    lumen_msg_hdr_t hdr = *hdrp;

    memset(ev, 0, sizeof(*ev));
    ev->type = hdr.op;

    switch (hdr.op) {
    case LUMEN_EV_KEY: {
        lumen_key_event_t k;
        if (lumen_read_full(fd, &k, sizeof(k)) != 0) return -1;
        ev->window_id     = k.window_id;
        ev->key.keycode   = k.keycode;
        ev->key.modifiers = k.modifiers;
        ev->key.pressed   = k.pressed;
        break;
    }
    case LUMEN_EV_MOUSE: {
        lumen_mouse_event_t m;
        if (lumen_read_full(fd, &m, sizeof(m)) != 0) return -1;
        ev->window_id      = m.window_id;
        ev->mouse.x        = m.x;
        ev->mouse.y        = m.y;
        ev->mouse.buttons  = m.buttons;
        ev->mouse.evtype   = m.evtype;
        ev->mouse.scroll   = m.scroll;
        break;
    }
    case LUMEN_EV_CLOSE_REQUEST: {
        lumen_close_request_t c;
        if (lumen_read_full(fd, &c, sizeof(c)) != 0) return -1;
        ev->window_id = c.window_id;
        break;
    }
    case LUMEN_EV_FOCUS: {
        lumen_focus_event_t f;
        if (lumen_read_full(fd, &f, sizeof(f)) != 0) return -1;
        ev->window_id    = f.window_id;
        ev->focus.focused = f.focused;
        break;
    }
    case LUMEN_EV_RESIZED: {
        lumen_resized_event_t r;
        if (lumen_read_full(fd, &r, sizeof(r)) != 0) return -1;
        ev->window_id     = r.window_id;
        ev->resized.new_w = r.new_width;
        ev->resized.new_h = r.new_height;
        break;
    }
    case LUMEN_EV_DRAG_OVER: {
        lumen_drag_over_t d;
        if (lumen_read_full(fd, &d, sizeof(d)) != 0) return -1;
        ev->window_id = d.window_id;
        ev->drag.x    = d.x;
        ev->drag.y    = d.y;
        ev->drag.op   = d.op;
        break;
    }
    case LUMEN_EV_DRAG_LEAVE: {
        lumen_drag_leave_t d;
        if (lumen_read_full(fd, &d, sizeof(d)) != 0) return -1;
        ev->window_id = d.window_id;
        break;
    }
    case LUMEN_EV_DROP: {
        lumen_drop_event_t d;
        if (lumen_read_full(fd, &d, sizeof(d)) != 0) return -1;
        ev->window_id = d.window_id;
        ev->drop.x    = d.x;
        ev->drop.y    = d.y;
        ev->drop.op   = d.op;
        memcpy(ev->drop.path, d.path, sizeof(ev->drop.path));
        ev->drop.path[sizeof(ev->drop.path) - 1] = '\0';
        break;
    }
    case LUMEN_EV_WINDOW_LIST: {
        static lumen_window_info_t s_wins[16];
        int count = (int)(hdr.len / sizeof(lumen_window_info_t));
        int cap = count > 16 ? 16 : count;
        for (int i = 0; i < cap; i++)
            if (lumen_read_full(fd, &s_wins[i], sizeof(s_wins[i])) != 0) return -1;
        uint32_t rem = hdr.len - (uint32_t)cap * sizeof(lumen_window_info_t);
        char tmp[256];
        while (rem > 0) {
            ssize_t r = read(fd, tmp,
                             rem < (uint32_t)sizeof(tmp) ? rem : (uint32_t)sizeof(tmp));
            if (r <= 0) return -1;
            rem -= (uint32_t)r;
        }
        ev->windows.count = cap;
        ev->windows.items = s_wins;
        break;
    }
    case LUMEN_EV_MENU_INVOKE: {
        lumen_menu_invoke_t mi;
        if (lumen_read_full(fd, &mi, sizeof(mi)) != 0) return -1;
        ev->window_id   = mi.window_id;
        ev->menu.command = mi.command;
        break;
    }
    case LUMEN_EV_MENU_STATE: {
        /* Large (multi-KB) fixed frame, same shape as SET_MENU — persists
         * until the next call, like WINDOW_LIST's s_wins. Read exactly hdr.len
         * bytes (draining any excess) — NOT sizeof — so a client/server struct-
         * size skew can't desync the stream: reading sizeof while the sender
         * framed hdr.len leaves leftover bytes that mis-frame every later
         * message (the top-bar-click bug — the shell then read op=0 garbage
         * forever and never saw the mouse event). */
        static lumen_set_menu_t s_menu_state;
        memset(&s_menu_state, 0, sizeof(s_menu_state));
        uint32_t want = hdr.len < sizeof(s_menu_state) ? hdr.len : (uint32_t)sizeof(s_menu_state);
        if (want && lumen_read_full(fd, &s_menu_state, want) != 0) return -1;
        uint32_t rem = hdr.len - want;
        char tmp[256];
        while (rem > 0) {
            ssize_t r = read(fd, tmp, rem < (uint32_t)sizeof(tmp) ? rem : (uint32_t)sizeof(tmp));
            if (r <= 0) return -1;
            rem -= (uint32_t)r;
        }
        ev->window_id      = s_menu_state.window_id;
        ev->menu_state.menu = &s_menu_state;
        break;
    }
    default: {
        char tmp[256];
        uint32_t rem = hdr.len;
        while (rem > 0) {
            ssize_t r = read(fd, tmp,
                             rem < (uint32_t)sizeof(tmp) ? rem : (uint32_t)sizeof(tmp));
            if (r <= 0) return -1;
            rem -= (uint32_t)r;
        }
        ev->type = 0;
        break;
    }
    }
    return 1;
}

/* Receive lumen_window_created_t reply + SCM_RIGHTS memfd, mmap, build win.
 * Used by both lumen_window_create() and lumen_panel_create(). */
static lumen_window_t *recv_created_reply(int fd, const char *what)
{
    lumen_window_created_t created;

    /* Consumes the reply header for us (op==0) — only the body+cmsg are
     * left to read below. See drain_interleaved_events's header comment. */
    if (drain_interleaved_events(fd) != 0) {
        lumen_diag("[LUMEN-CLI] %s: drain_interleaved_events failed\n", what);
        return NULL;
    }

    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { .iov_base = &created, .iov_len = sizeof(created) };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    /* recvmsg can return SHORT on a stream socket — it is not obliged to hand
     * back the whole 24-byte body in one call. Leaving the tail unread put the
     * next header read in the middle of this reply, and from then on the client
     * parses body bytes as headers (mostly zeros: op=0/len=0, which consumes
     * nothing) and spins with its UI frozen. That was the top-bar freeze. Take
     * the cmsg with the first chunk, then read the rest to completion. */
    ssize_t got = recvmsg(fd, &msg, 0);
    if (got < 0) {
        lumen_diag("[LUMEN-CLI] %s: recvmsg errno=%d\n", what, errno);
        return NULL;
    }
    if ((size_t)got < sizeof(created) &&
        lumen_read_full(fd, (uint8_t *)&created + got,
                        sizeof(created) - (size_t)got) != 0) {
        lumen_diag("[LUMEN-CLI] %s: short reply (%zd of %zu)\n",
                   what, got, sizeof(created));
        return NULL;
    }
    if (created.status != 0) {
        lumen_diag("[LUMEN-CLI] %s: server status=%u\n", what, created.status);
        return NULL;
    }

    int memfd = -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
        cmsg->cmsg_type == SCM_RIGHTS)
        memcpy(&memfd, CMSG_DATA(cmsg), sizeof(int));
    if (memfd < 0) {
        lumen_diag("[LUMEN-CLI] %s: no memfd in cmsg\n", what);
        return NULL;
    }

    size_t bufsz = (size_t)created.width * created.height * sizeof(uint32_t);

    void *shared = mmap(NULL, bufsz, PROT_READ | PROT_WRITE,
                        MAP_SHARED, memfd, 0);
    if (shared == MAP_FAILED) {
        lumen_diag("[LUMEN-CLI] %s: mmap errno=%d bufsz=%lu\n",
            what, errno, (unsigned long)bufsz);
        close(memfd); return NULL;
    }

    void *backbuf = malloc(bufsz);
    if (!backbuf) { munmap(shared, bufsz); close(memfd); return NULL; }
    memset(backbuf, 0, bufsz);

    lumen_window_t *win = malloc(sizeof(*win));
    if (!win) { free(backbuf); munmap(shared, bufsz); close(memfd); return NULL; }

    win->fd      = fd;
    win->id      = created.window_id;
    win->memfd   = memfd;
    win->shared  = shared;
    win->backbuf = backbuf;
    win->w       = (int)created.width;
    win->h       = (int)created.height;
    win->stride  = (int)created.width;
    win->x       = created.x;
    win->y       = created.y;

    return win;
}

lumen_window_t *lumen_window_create(int fd, const char *title, int w, int h)
{
    return lumen_window_create_ex(fd, title, w, h, 0);
}

lumen_window_t *lumen_window_create_ex(int fd, const char *title,
                                       int w, int h, unsigned flags)
{
    lumen_msg_hdr_t hdr = { LUMEN_OP_CREATE_WINDOW,
                             sizeof(lumen_create_window_t) };
    lumen_create_window_t req;
    memset(&req, 0, sizeof(req));
    req.width  = (uint16_t)w;
    req.height = (uint16_t)h;
    req.flags  = (uint16_t)flags;
    strncpy(req.title, title ? title : "", sizeof(req.title) - 1);

    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        lumen_diag("[LUMEN-CLI] window_create: write hdr errno=%d\n", errno);
        return NULL;
    }
    if (write(fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        lumen_diag("[LUMEN-CLI] window_create: write req errno=%d\n", errno);
        return NULL;
    }
    return recv_created_reply(fd, "window_create");
}

lumen_window_t *lumen_panel_create(int fd, int w, int h)
{
    return lumen_panel_create_anchored(fd, w, h, LUMEN_PANEL_BOTTOM);
}

lumen_window_t *lumen_panel_create_anchored(int fd, int w, int h, unsigned anchor)
{
    lumen_msg_hdr_t hdr = { LUMEN_OP_CREATE_PANEL,
                             sizeof(lumen_create_panel_t) };
    lumen_create_panel_t req;
    memset(&req, 0, sizeof(req));
    req.width  = (uint16_t)w;
    req.height = (uint16_t)h;
    req.anchor = (uint16_t)anchor;

    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        lumen_diag("[LUMEN-CLI] panel_create: write hdr errno=%d\n", errno);
        return NULL;
    }
    if (write(fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        lumen_diag("[LUMEN-CLI] panel_create: write req errno=%d\n", errno);
        return NULL;
    }
    return recv_created_reply(fd, "panel_create");
}

int lumen_drag_start(lumen_window_t *win, int op,
                     const char *label, const char *path)
{
    if (!win || !path || !path[0]) return -EINVAL;
    lumen_msg_hdr_t hdr = { LUMEN_OP_DRAG_START, sizeof(lumen_drag_start_t) };
    lumen_drag_start_t req;
    memset(&req, 0, sizeof(req));
    req.window_id = win->id;
    req.op = (uint8_t)op;
    if (label) strncpy(req.label, label, sizeof(req.label) - 1);
    strncpy(req.path, path, sizeof(req.path) - 1);
    if (write(win->fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        lumen_diag("[LUMEN-CLI] drag_start: write hdr errno=%d\n", errno);
        return -EIO;
    }
    if (write(win->fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        lumen_diag("[LUMEN-CLI] drag_start: write req errno=%d\n", errno);
        return -EIO;
    }
    return 0;
}

int lumen_invoke_focused_menu(int fd, uint32_t command)
{
    lumen_msg_hdr_t hdr = { LUMEN_OP_INVOKE_FOCUSED_MENU,
                             sizeof(lumen_invoke_focused_menu_t) };
    lumen_invoke_focused_menu_t req = { command };
    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        lumen_diag("[LUMEN-CLI] invoke_focused_menu: write hdr errno=%d\n", errno);
        return -EIO;
    }
    if (write(fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        lumen_diag("[LUMEN-CLI] invoke_focused_menu: write req errno=%d\n", errno);
        return -EIO;
    }
    return 0;
}

int lumen_invoke(int fd, const char *name)
{
    lumen_msg_hdr_t hdr = { LUMEN_OP_INVOKE, sizeof(lumen_invoke_t) };
    lumen_invoke_t req;
    memset(&req, 0, sizeof(req));
    if (name) strncpy(req.name, name, sizeof(req.name) - 1);
    if (write(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        lumen_diag("[LUMEN-CLI] invoke: write hdr errno=%d\n", errno);
        return -EIO;
    }
    if (write(fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        lumen_diag("[LUMEN-CLI] invoke: write req errno=%d\n", errno);
        return -EIO;
    }
    return 0;
}

void lumen_window_set_admin(lumen_window_t *win, int admin)
{
    if (!win) return;
    lumen_msg_hdr_t hdr = { LUMEN_OP_SET_ADMIN, sizeof(lumen_set_admin_t) };
    lumen_set_admin_t req = { win->id, admin ? 1u : 0u };
    if (write(win->fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        lumen_diag("[LUMEN-CLI] set_admin: write hdr errno=%d\n", errno);
        return;
    }
    if (write(win->fd, &req, sizeof(req)) != (ssize_t)sizeof(req))
        lumen_diag("[LUMEN-CLI] set_admin: write req errno=%d\n", errno);
}

/* ── App menu bar ───────────────────────────────────────────────────── */

void glyph_menu_reset(lumen_set_menu_t *m, uint32_t window_id)
{
    memset(m, 0, sizeof(*m));
    m->window_id = window_id;
}

int glyph_menu_add_col(lumen_set_menu_t *m, const char *title)
{
    if (m->col_count >= LUMEN_MENU_MAX_COLS) return -1;
    int c = m->col_count++;
    strncpy(m->cols[c].title, title, LUMEN_MENU_LABEL_LEN - 1);
    m->cols[c].item_count = 0;
    return c;
}

void glyph_menu_add_item(lumen_set_menu_t *m, int col, const char *label,
                         uint32_t command)
{
    if (col < 0 || col >= (int)m->col_count) return;
    lumen_menu_col_t *c = &m->cols[col];
    if (c->item_count >= LUMEN_MENU_MAX_ITEMS) return;
    lumen_menu_item_t *it = &c->items[c->item_count++];
    strncpy(it->label, label ? label : "", LUMEN_MENU_LABEL_LEN - 1);
    it->command = command;
}

/* A separator is just an item with an empty label. */
void glyph_menu_add_sep(lumen_set_menu_t *m, int col)
{
    glyph_menu_add_item(m, col, "", 0);
}

void lumen_window_set_menu(lumen_window_t *win, const lumen_set_menu_t *menu)
{
    if (!win || !menu) return;
    lumen_set_menu_t req = *menu;
    req.window_id = win->id;   /* authoritative — ignore any caller value */
    lumen_msg_hdr_t hdr = { LUMEN_OP_SET_MENU, sizeof(req) };
    /* SET_MENU is large (multi-KB) — loop over short writes so the frame isn't
     * truncated (which would desync the server and drop the connection). */
    if (lumen_write_full(win->fd, &hdr, sizeof(hdr)) != 0 ||
        lumen_write_full(win->fd, &req, sizeof(req)) != 0)
        lumen_diag("[LUMEN-CLI] set_menu: write errno=%d\n", errno);
}

void lumen_window_present(lumen_window_t *win)
{
    size_t bufsz = (size_t)win->w * win->h * sizeof(uint32_t);
    memcpy(win->shared, win->backbuf, bufsz);

    lumen_msg_hdr_t hdr = { LUMEN_OP_DAMAGE, sizeof(lumen_damage_t) };
    lumen_damage_t dmg  = { win->id };
    write(win->fd, &hdr, sizeof(hdr));
    write(win->fd, &dmg, sizeof(dmg));
}

/* Apply a server-initiated resize (received as LUMEN_EV_RESIZED). Asks the
 * server for a fresh buffer at new_w×new_h and remaps shared+backbuf in place;
 * win->w/h/stride are updated. Returns 0 on success, -1 on failure (window
 * left at its old size). The caller must rebuild its surface_t (it points at
 * the new backbuf) and repaint. */
/* Receive a resize reply (lumen_window_created_t + new memfd via SCM_RIGHTS) and
 * remap win->shared/backbuf in place. Shared by apply_resize (server-initiated)
 * and resize_self (client-initiated) — both send a request, then read this.
 * drain_interleaved_events() handles the server pushing an async event (e.g.
 * LUMEN_EV_MENU_STATE) on this same connection before the reply arrives. */
static int recv_buffer_reply(lumen_window_t *win)
{
    lumen_window_created_t created;

    /* Consumes the reply header for us (op==0) — only the body+cmsg are
     * left to read below. See drain_interleaved_events's header comment. */
    if (drain_interleaved_events(win->fd) != 0) {
        lumen_diag("[LUMEN-CLI] resize: drain_interleaved_events failed\n");
        return -1;
    }

    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { .iov_base = &created, .iov_len = sizeof(created) };
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf; msg.msg_controllen = sizeof(cmsgbuf);
    /* Short recvmsg: same hazard as recv_created_reply — finish the body or
     * the stream is desynced for good. This is the hot path (the shell resizes
     * its panel on every dropdown, while multi-KB menu pushes are in flight),
     * so it is where the top bar actually froze. */
    ssize_t got = recvmsg(win->fd, &msg, 0);
    if (got < 0) {
        lumen_diag("[LUMEN-CLI] resize: recvmsg errno=%d\n", errno);
        return -1;
    }
    if ((size_t)got < sizeof(created) &&
        lumen_read_full(win->fd, (uint8_t *)&created + got,
                        sizeof(created) - (size_t)got) != 0) {
        lumen_diag("[LUMEN-CLI] resize: short reply (%zd of %zu)\n",
                   got, sizeof(created));
        return -1;
    }
    if (created.status != 0) {
        lumen_diag("[LUMEN-CLI] resize: server status=%u\n", created.status);
        return -1;
    }

    int memfd = -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
        memcpy(&memfd, CMSG_DATA(cmsg), sizeof(int));
    if (memfd < 0) { lumen_diag("[LUMEN-CLI] resize: no memfd\n"); return -1; }

    size_t nbufsz = (size_t)created.width * created.height * sizeof(uint32_t);
    void *nshared = mmap(NULL, nbufsz, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (nshared == MAP_FAILED) { close(memfd); return -1; }
    void *nback = malloc(nbufsz);
    if (!nback) { munmap(nshared, nbufsz); close(memfd); return -1; }
    memset(nback, 0, nbufsz);

    munmap(win->shared, (size_t)win->w * win->h * sizeof(uint32_t));
    close(win->memfd);
    free(win->backbuf);
    win->memfd  = memfd;
    win->shared = nshared;
    win->backbuf = nback;
    win->w = (int)created.width;
    win->h = (int)created.height;
    win->stride = (int)created.width;
    win->x = created.x;
    win->y = created.y;
    return 0;
}

/* ponytail: assumes the reply is the next message on the socket (the server
 * handles it synchronously, like CREATE_WINDOW). True today; if the server ever
 * interleaves events before the reply, this needs a queue. */
int lumen_window_apply_resize(lumen_window_t *win, int new_w, int new_h)
{
    if (!win || new_w < 1 || new_h < 1) return -1;
    lumen_msg_hdr_t hdr = { LUMEN_OP_RESIZE_BUFFER, sizeof(lumen_resize_buffer_t) };
    lumen_resize_buffer_t req = { win->id, (uint32_t)new_w, (uint32_t)new_h };
    if (write(win->fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
        write(win->fd, &req, sizeof(req)) != (ssize_t)sizeof(req)) {
        lumen_diag("[LUMEN-CLI] resize: write errno=%d\n", errno);
        return -1;
    }
    return recv_buffer_reply(win);
}

int lumen_window_resize_self(lumen_window_t *win, int w, int h)
{
    if (!win || w < 1 || h < 1) return -1;
    lumen_msg_hdr_t hdr = { LUMEN_OP_RESIZE_SELF, sizeof(lumen_resize_self_t) };
    lumen_resize_self_t req = { (uint32_t)w, (uint32_t)h };
    if (write(win->fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
        write(win->fd, &req, sizeof(req)) != (ssize_t)sizeof(req))
        return -1;
    return recv_buffer_reply(win);
}

void lumen_activate_window(int fd, uint32_t gid)
{
    lumen_msg_hdr_t hdr = { LUMEN_OP_ACTIVATE_WINDOW, sizeof(lumen_activate_window_t) };
    lumen_activate_window_t req = { gid };
    write(fd, &hdr, sizeof(hdr));
    write(fd, &req, sizeof(req));
}

int lumen_poll_event(int fd, lumen_event_t *ev)
{
    if (ev_queue_pop(ev)) return 1;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 0) <= 0) return 0;
    return recv_event(fd, ev);
}

int lumen_wait_event(int fd, lumen_event_t *ev, int timeout_ms)
{
    if (ev_queue_pop(ev)) return 1;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int r = poll(&pfd, 1, timeout_ms);
    if (r == 0) return 0;
    if (r < 0)  return -1;
    return recv_event(fd, ev);
}

void lumen_window_destroy(lumen_window_t *win)
{
    lumen_msg_hdr_t hdr  = { LUMEN_OP_DESTROY_WINDOW,
                              sizeof(lumen_destroy_window_t) };
    lumen_destroy_window_t req = { win->id };
    write(win->fd, &hdr, sizeof(hdr));
    write(win->fd, &req, sizeof(req));

    size_t bufsz = (size_t)win->w * win->h * sizeof(uint32_t);
    munmap(win->shared, bufsz);
    close(win->memfd);
    free(win->backbuf);
    free(win);
}
