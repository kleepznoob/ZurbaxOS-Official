/* Mail: accounts, IMAP and SMTP, MIME, and the message cache. */
#pragma once

#include "zui.h"
#include "zn.h"
#include "../drift/drift.h"

/* ---- MIME ------------------------------------------------------------------------------------ */

typedef struct Part Part;
struct Part {
	char type[64];           /* lower case, "text/plain" */
	char charset[32];
	char encoding[24];       /* content-transfer-encoding */
	char filename[160];
	char cid[160];           /* Content-ID without <> */
	bool attachment;
	const char *headers;     /* points into the message */
	usize headers_len;
	const char *body;        /* raw (still encoded) */
	usize body_len;
	Part *kids;
	int nkids;
};

/* Parses a whole message (headers and body). The message must outlive the parts. */
Part *mime_parse(const char *msg, usize len);
void mime_free(Part *p);
/* The decoded body of a part, as UTF-8 for text and raw bytes otherwise. */
char *mime_body(const Part *p, usize *len);
/* An unfolded header value, or NULL. */
char *mime_header(const char *headers, usize len, const char *name);
/* RFC 2047 encoded words ("=?utf-8?B?...?=") to UTF-8. */
char *mime_words(const char *s);
/* Latin-1 / Windows-1252 / UTF-8 text to UTF-8. */
char *mime_to_utf8(const char *s, usize n, const char *charset, usize *out_len);
/* The part to show: HTML if there is one, else plain text. */
const Part *mime_best_text(const Part *p, bool *is_html);
void mime_attachments(const Part *p, const Part **out, int *n, int max);
const Part *mime_find_cid(const Part *p, const char *cid);
/* "Tue, 6 Oct 2026 10:12:33 +0200" or "06-Oct-2026 10:12:33 +0200" to Unix time. */
i64 mime_date(const char *s);
/* Splits "Name <addr>" into its parts (decoded). */
void mime_address(const char *s, char *name, usize ncap, char *addr, usize acap);
/* Encodes a header value as encoded words if it isn't plain ASCII. */
char *mime_encode_header(const char *s);
char *mime_quoted_printable(const char *s, usize n);

/* ---- Accounts --------------------------------------------------------------------------------- */

typedef struct {
	char name[96];           /* your name, for From: */
	char email[128];
	char password[256];
	char imap_host[128], smtp_host[128];
	u16 imap_port, smtp_port;
} Account;

/* Fills in servers for well-known providers from the address. */
void account_guess(Account *a);

/* ---- IMAP ------------------------------------------------------------------------------------- */

typedef struct Imap Imap;

typedef struct {
	char name[256];          /* as the server knows it (modified UTF-7 decoded for show) */
	char raw[256];           /* as the server knows it */
	char role[16];           /* inbox, sent, drafts, trash, junk, archive, flagged, or "" */
	int unseen;
} Folder;

typedef struct {
	u32 uid;
	char from_name[96], from_addr[128];
	char to[256];
	char subject[256];
	char snippet[200];
	char message_id[200];
	i64 date;
	bool seen, flagged, answered;
	u32 size;
} Head;

Imap *imap_open(const Account *a, char *err, usize errcap);
void imap_close(Imap *m);
int imap_folders(Imap *m, Folder *out, int max);
/* Selects a folder and reads the newest `max` messages, newest first. */
int imap_heads(Imap *m, const char *folder, Head *out, int max, u32 *exists);
/* The whole message. */
char *imap_message(Imap *m, const char *folder, u32 uid, usize *len);
bool imap_flag(Imap *m, const char *folder, u32 uid, const char *flag, bool on);
bool imap_move(Imap *m, const char *folder, u32 uid, const char *to);
/* Adds a message to a folder with flags like "\\Seen \\Draft". */
bool imap_append(Imap *m, const char *folder, const char *flags, const char *msg, usize len);
bool imap_delete(Imap *m, const char *folder, u32 uid);
/* Messages containing every word of the query, newest first. */
int imap_search(Imap *m, const char *folder, const char *query, Head *out, int max);
/* Unread messages in a folder, or -1. */
int imap_unseen(Imap *m, const char *folder);
/* Whether the connection still works (sends NOOP). */
bool imap_alive(Imap *m);
bool imap_is_gmail(Imap *m);
/* The connection broke (the server hung up or went quiet). */
bool imap_dead(Imap *m);
const char *imap_error(Imap *m);

/* ---- SMTP ------------------------------------------------------------------------------------- */

/* Sends a finished message to the recipients. False with a reason on failure. */
bool smtp_send(const Account *a, const char *const *rcpt, int nrcpt, const char *msg, usize len, char *err, usize errcap);

/* ---- Showing a message -------------------------------------------------------------------------- */

/* A parsed message and the page Drift's engine makes of its body. */
typedef struct {
	Doc doc;                 /* first, so a Doc* is a Msg* */
	char *raw;
	usize len;
	Part *mime;
	bool html;               /* the body was HTML (shown on white) */
	char *text;              /* the body as plain text, for quoting */
	char subject[256];
	char from_name[96], from_addr[128];
	char reply_to[256];
	char to[1024], cc[1024];
	char message_id[200], references[1024];
	i64 date;
	const Part *atts[32];
	int natts;
	int remote;              /* atomic: pictures from the web that were held back */
	bool allow_remote;
	f32 laid_w;
	int laid_images;
	f64 laid_at;
} Msg;

typedef struct {
	Color text, link, background, quote;
} MsgColors;

/* Parses a message; colors are for plain text messages. */
Msg *msg_new(const char *raw, usize len, const MsgColors *colors);
void msg_ref(Msg *m);
void msg_release(Msg *m);
/* Lays the body out for this width on a helper thread (if it changed). */
void msg_layout(Ui *ui, Msg *m, f32 width);
/* Takes a finished layout; true if the page changed. */
bool msg_take_page(Msg *m);
/* Lets pictures from the web load. */
void msg_allow_remote(Msg *m);
void view_start(Ui *ui);
