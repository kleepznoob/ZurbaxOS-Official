/* Chess: you against the computer.
 *
 * The board is 64 squares (a1 = 0, h8 = 63). The computer searches with
 * alpha-beta, iterative deepening and a quiescence search over captures,
 * on its own thread so the window stays responsive while it thinks. */
#include "zui.h"

enum { EMPTY, PAWN, KNIGHT, BISHOP, ROOK, QUEEN, KING };
enum { WHITE = 0, BLACK = 8 };
#define TYPE(p) ((p) & 7)
#define COLOR(p) ((p) & 8)

enum { MF_CAPTURE = 1, MF_EP = 2, MF_CASTLE = 4, MF_DOUBLE = 8 };
enum { WK = 1, WQ = 2, BK = 4, BQ = 8 };

typedef struct {
	u8 from, to, promo, flags;
} Move;

typedef struct {
	u8 sq[64];
	u8 side;                 /* WHITE or BLACK to move */
	u8 castle;
	i8 ep;                   /* en passant target square, or -1 */
	u16 halfmove;
	u64 key;
} Pos;

typedef struct {
	Move m;
	u8 captured, castle, halfmove;
	i8 ep;
	u64 key;
} Undo;

/* ---- Zobrist keys --------------------------------------------------------------------------- */

static u64 zob_piece[16][64], zob_side, zob_castle[16], zob_ep[64];

static u64 splitmix(u64 *s)
{
	u64 z = (*s += 0x9E3779B97F4A7C15ull);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

static void zobrist_init(void)
{
	u64 s = 20260101;
	for (int p = 0; p < 16; p++)
		for (int q = 0; q < 64; q++)
			zob_piece[p][q] = splitmix(&s);
	zob_side = splitmix(&s);
	for (int i = 0; i < 16; i++)
		zob_castle[i] = splitmix(&s);
	for (int i = 0; i < 64; i++)
		zob_ep[i] = splitmix(&s);
}

static u64 compute_key(const Pos *p)
{
	u64 k = 0;
	for (int i = 0; i < 64; i++)
		if (p->sq[i])
			k ^= zob_piece[p->sq[i]][i];
	if (p->side == BLACK)
		k ^= zob_side;
	k ^= zob_castle[p->castle];
	if (p->ep >= 0)
		k ^= zob_ep[p->ep];
	return k;
}

static void pos_start(Pos *p)
{
	static const u8 back[8] = { ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK };
	memset(p, 0, sizeof *p);
	for (int f = 0; f < 8; f++) {
		p->sq[f] = back[f] | WHITE;
		p->sq[8 + f] = PAWN | WHITE;
		p->sq[48 + f] = PAWN | BLACK;
		p->sq[56 + f] = back[f] | BLACK;
	}
	p->side = WHITE;
	p->castle = WK | WQ | BK | BQ;
	p->ep = -1;
	p->key = compute_key(p);
}

/* ---- Move generation ------------------------------------------------------------------------ */

static const int KNIGHT_D[8][2] = { { 1, 2 }, { 2, 1 }, { 2, -1 }, { 1, -2 }, { -1, -2 }, { -2, -1 }, { -2, 1 }, { -1, 2 } };
static const int KING_D[8][2] = { { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 } };
static const int ROOK_D[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
static const int BISHOP_D[4][2] = { { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };

static int at(int f, int r)
{
	return (f < 0 || f > 7 || r < 0 || r > 7) ? -1 : r * 8 + f;
}

/* Is square s attacked by side `by`? */
static bool attacked(const Pos *p, int s, int by)
{
	int f = s & 7, r = s >> 3;
	int dir = by == WHITE ? -1 : 1;   /* pawns of `by` sit one rank behind */
	for (int df = -1; df <= 1; df += 2) {
		int q = at(f + df, r + dir);
		if (q >= 0 && p->sq[q] == (PAWN | by))
			return true;
	}
	for (int i = 0; i < 8; i++) {
		int q = at(f + KNIGHT_D[i][0], r + KNIGHT_D[i][1]);
		if (q >= 0 && p->sq[q] == (KNIGHT | by))
			return true;
		q = at(f + KING_D[i][0], r + KING_D[i][1]);
		if (q >= 0 && p->sq[q] == (KING | by))
			return true;
	}
	for (int i = 0; i < 4; i++) {
		for (int k = 1;; k++) {
			int q = at(f + ROOK_D[i][0] * k, r + ROOK_D[i][1] * k);
			if (q < 0)
				break;
			u8 pc = p->sq[q];
			if (pc) {
				if (COLOR(pc) == by && (TYPE(pc) == ROOK || TYPE(pc) == QUEEN))
					return true;
				break;
			}
		}
		for (int k = 1;; k++) {
			int q = at(f + BISHOP_D[i][0] * k, r + BISHOP_D[i][1] * k);
			if (q < 0)
				break;
			u8 pc = p->sq[q];
			if (pc) {
				if (COLOR(pc) == by && (TYPE(pc) == BISHOP || TYPE(pc) == QUEEN))
					return true;
				break;
			}
		}
	}
	return false;
}

static int king_square(const Pos *p, int side)
{
	for (int i = 0; i < 64; i++)
		if (p->sq[i] == (KING | side))
			return i;
	return -1;
}

static bool in_check(const Pos *p, int side)
{
	int k = king_square(p, side);
	return k >= 0 && attacked(p, k, side ^ 8);
}

static void add(Move *list, int *n, int from, int to, int promo, int flags)
{
	list[(*n)++] = (Move){ (u8)from, (u8)to, (u8)promo, (u8)flags };
}

/* Pseudo-legal moves (the king may be left in check). captures_only for quiescence. */
static int gen_moves(const Pos *p, Move *list, bool captures_only)
{
	int n = 0, us = p->side, them = us ^ 8;
	for (int s = 0; s < 64; s++) {
		u8 pc = p->sq[s];
		if (!pc || COLOR(pc) != us)
			continue;
		int f = s & 7, r = s >> 3;
		switch (TYPE(pc)) {
		case PAWN: {
			int dir = us == WHITE ? 1 : -1, start = us == WHITE ? 1 : 6, last = us == WHITE ? 7 : 0;
			int q = at(f, r + dir);
			if (q >= 0 && !p->sq[q]) {
				if (r + dir == last) {
					for (int pr = QUEEN; pr >= KNIGHT; pr--)
						add(list, &n, s, q, pr, 0);
				} else if (!captures_only) {
					add(list, &n, s, q, 0, 0);
					int q2 = at(f, r + 2 * dir);
					if (r == start && !p->sq[q2])
						add(list, &n, s, q2, 0, MF_DOUBLE);
				}
			}
			for (int df = -1; df <= 1; df += 2) {
				int c = at(f + df, r + dir);
				if (c < 0)
					continue;
				if (p->sq[c] && COLOR(p->sq[c]) == them) {
					if (r + dir == last)
						for (int pr = QUEEN; pr >= KNIGHT; pr--)
							add(list, &n, s, c, pr, MF_CAPTURE);
					else
						add(list, &n, s, c, 0, MF_CAPTURE);
				} else if (c == p->ep) {
					add(list, &n, s, c, 0, MF_CAPTURE | MF_EP);
				}
			}
			break;
		}
		case KNIGHT:
		case KING: {
			const int(*d)[2] = TYPE(pc) == KNIGHT ? KNIGHT_D : KING_D;
			for (int i = 0; i < 8; i++) {
				int q = at(f + d[i][0], r + d[i][1]);
				if (q < 0)
					continue;
				if (!p->sq[q]) {
					if (!captures_only)
						add(list, &n, s, q, 0, 0);
				} else if (COLOR(p->sq[q]) == them) {
					add(list, &n, s, q, 0, MF_CAPTURE);
				}
			}
			if (TYPE(pc) == KING && !captures_only) {
				int home = us == WHITE ? 4 : 60;
				int kside = us == WHITE ? WK : BK, qside = us == WHITE ? WQ : BQ;
				if (s == home && (p->castle & kside) && !p->sq[home + 1] && !p->sq[home + 2] &&
				    !attacked(p, home, them) && !attacked(p, home + 1, them) && !attacked(p, home + 2, them))
					add(list, &n, s, home + 2, 0, MF_CASTLE);
				if (s == home && (p->castle & qside) && !p->sq[home - 1] && !p->sq[home - 2] && !p->sq[home - 3] &&
				    !attacked(p, home, them) && !attacked(p, home - 1, them) && !attacked(p, home - 2, them))
					add(list, &n, s, home - 2, 0, MF_CASTLE);
			}
			break;
		}
		default: {
			bool diag = TYPE(pc) == BISHOP || TYPE(pc) == QUEEN, straight = TYPE(pc) == ROOK || TYPE(pc) == QUEEN;
			for (int i = 0; i < 4; i++) {
				for (int pass = 0; pass < 2; pass++) {
					if ((pass == 0 && !straight) || (pass == 1 && !diag))
						continue;
					const int *d = pass == 0 ? ROOK_D[i] : BISHOP_D[i];
					for (int k = 1;; k++) {
						int q = at(f + d[0] * k, r + d[1] * k);
						if (q < 0)
							break;
						if (!p->sq[q]) {
							if (!captures_only)
								add(list, &n, s, q, 0, 0);
							continue;
						}
						if (COLOR(p->sq[q]) == them)
							add(list, &n, s, q, 0, MF_CAPTURE);
						break;
					}
				}
			}
		}
		}
	}
	return n;
}

static const u8 CASTLE_MASK[64] = {
	[0] = WQ, [4] = WK | WQ, [7] = WK, [56] = BQ, [60] = BK | BQ, [63] = BK,
};

static void make_move(Pos *p, Move m, Undo *u)
{
	u->m = m;
	u->captured = p->sq[m.to];
	u->castle = p->castle;
	u->ep = p->ep;
	u->halfmove = (u8)MIN(p->halfmove, 255);
	u->key = p->key;
	u8 pc = p->sq[m.from];
	u64 k = p->key;
	k ^= zob_piece[pc][m.from];
	if (u->captured)
		k ^= zob_piece[u->captured][m.to];
	if (m.flags & MF_EP) {
		int cap = m.to + (p->side == WHITE ? -8 : 8);
		u->captured = p->sq[cap];
		k ^= zob_piece[p->sq[cap]][cap];
		p->sq[cap] = 0;
	}
	p->sq[m.from] = 0;
	u8 placed = m.promo ? (u8)(m.promo | p->side) : pc;
	p->sq[m.to] = placed;
	k ^= zob_piece[placed][m.to];
	if (m.flags & MF_CASTLE) {
		int rf = m.to > m.from ? m.from + 3 : m.from - 4, rt = m.to > m.from ? m.from + 1 : m.from - 1;
		u8 rook = p->sq[rf];
		p->sq[rt] = rook;
		p->sq[rf] = 0;
		k ^= zob_piece[rook][rf] ^ zob_piece[rook][rt];
	}
	k ^= zob_castle[p->castle];
	p->castle &= (u8) ~(CASTLE_MASK[m.from] | CASTLE_MASK[m.to]);
	k ^= zob_castle[p->castle];
	if (p->ep >= 0)
		k ^= zob_ep[p->ep];
	p->ep = (m.flags & MF_DOUBLE) ? (i8)((m.from + m.to) / 2) : -1;
	if (p->ep >= 0)
		k ^= zob_ep[p->ep];
	p->halfmove = (TYPE(pc) == PAWN || (m.flags & MF_CAPTURE)) ? 0 : p->halfmove + 1;
	p->side ^= 8;
	k ^= zob_side;
	p->key = k;
}

static void unmake_move(Pos *p, const Undo *u)
{
	Move m = u->m;
	p->side ^= 8;
	u8 pc = m.promo ? (u8)(PAWN | p->side) : p->sq[m.to];
	p->sq[m.from] = pc;
	if (m.flags & MF_EP) {
		p->sq[m.to] = 0;
		p->sq[m.to + (p->side == WHITE ? -8 : 8)] = u->captured;
	} else {
		p->sq[m.to] = u->captured;
	}
	if (m.flags & MF_CASTLE) {
		int rf = m.to > m.from ? m.from + 3 : m.from - 4, rt = m.to > m.from ? m.from + 1 : m.from - 1;
		p->sq[rf] = p->sq[rt];
		p->sq[rt] = 0;
	}
	p->castle = u->castle;
	p->ep = u->ep;
	p->halfmove = u->halfmove;
	p->key = u->key;
}

static int legal_moves(Pos *p, Move *out)
{
	Move all[256];
	int n = gen_moves(p, all, false), k = 0;
	for (int i = 0; i < n; i++) {
		Undo u;
		make_move(p, all[i], &u);
		if (!in_check(p, p->side ^ 8))
			out[k++] = all[i];
		unmake_move(p, &u);
	}
	return k;
}

/* ---- Evaluation ------------------------------------------------------------------------------ */

static const int VALUE[7] = { 0, 100, 320, 330, 500, 900, 0 };

/* From white's side, a1 first. Black mirrors. */
static const i8 PST[7][64] = {
	[PAWN] = { 0, 0, 0, 0, 0, 0, 0, 0, 5, 10, 10, -20, -20, 10, 10, 5, 5, -5, -10, 0, 0, -10, -5, 5, 0, 0, 0, 20, 20, 0, 0, 0,
		   5, 5, 10, 25, 25, 10, 5, 5, 10, 10, 20, 30, 30, 20, 10, 10, 50, 50, 50, 50, 50, 50, 50, 50, 0, 0, 0, 0, 0, 0, 0, 0 },
	[KNIGHT] = { -50, -40, -30, -30, -30, -30, -40, -50, -40, -20, 0, 5, 5, 0, -20, -40, -30, 5, 10, 15, 15, 10, 5, -30,
		     -30, 0, 15, 20, 20, 15, 0, -30, -30, 5, 15, 20, 20, 15, 5, -30, -30, 0, 10, 15, 15, 10, 0, -30,
		     -40, -20, 0, 0, 0, 0, -20, -40, -50, -40, -30, -30, -30, -30, -40, -50 },
	[BISHOP] = { -20, -10, -10, -10, -10, -10, -10, -20, -10, 5, 0, 0, 0, 0, 5, -10, -10, 10, 10, 10, 10, 10, 10, -10,
		     -10, 0, 10, 10, 10, 10, 0, -10, -10, 5, 5, 10, 10, 5, 5, -10, -10, 0, 5, 10, 10, 5, 0, -10,
		     -10, 0, 0, 0, 0, 0, 0, -10, -20, -10, -10, -10, -10, -10, -10, -20 },
	[ROOK] = { 0, 0, 0, 5, 5, 0, 0, 0, -5, 0, 0, 0, 0, 0, 0, -5, -5, 0, 0, 0, 0, 0, 0, -5, -5, 0, 0, 0, 0, 0, 0, -5,
		   -5, 0, 0, 0, 0, 0, 0, -5, -5, 0, 0, 0, 0, 0, 0, -5, 5, 10, 10, 10, 10, 10, 10, 5, 0, 0, 0, 0, 0, 0, 0, 0 },
	[QUEEN] = { -20, -10, -10, -5, -5, -10, -10, -20, -10, 0, 5, 0, 0, 0, 0, -10, -10, 5, 5, 5, 5, 5, 0, -10,
		    0, 0, 5, 5, 5, 5, 0, -5, -5, 0, 5, 5, 5, 5, 0, -5, -10, 0, 5, 5, 5, 5, 0, -10, -10, 0, 0, 0, 0, 0, 0, -10,
		    -20, -10, -10, -5, -5, -10, -10, -20 },
	[KING] = { 20, 30, 10, 0, 0, 10, 30, 20, 20, 20, 0, 0, 0, 0, 20, 20, -10, -20, -20, -20, -20, -20, -20, -10,
		   -20, -30, -30, -40, -40, -30, -30, -20, -30, -40, -40, -50, -50, -40, -40, -30, -30, -40, -40, -50, -50, -40, -40, -30,
		   -30, -40, -40, -50, -50, -40, -40, -30, -30, -40, -40, -50, -50, -40, -40, -30 },
};

static const i8 KING_END[64] = {
	-50, -30, -30, -30, -30, -30, -30, -50, -30, -30, 0, 0, 0, 0, -30, -30, -30, -10, 20, 30, 30, 20, -10, -30,
	-30, -10, 30, 40, 40, 30, -10, -30, -30, -10, 30, 40, 40, 30, -10, -30, -30, -10, 20, 30, 30, 20, -10, -30,
	-30, -20, -10, 0, 0, -10, -20, -30, -50, -40, -30, -20, -20, -30, -40, -50,
};

/* Score from the side to move's point of view. */
static int evaluate(const Pos *p)
{
	int score = 0, material = 0;
	for (int s = 0; s < 64; s++)
		if (p->sq[s] && TYPE(p->sq[s]) != PAWN && TYPE(p->sq[s]) != KING)
			material += VALUE[TYPE(p->sq[s])];
	bool endgame = material <= 1600;
	for (int s = 0; s < 64; s++) {
		u8 pc = p->sq[s];
		if (!pc)
			continue;
		int t = TYPE(pc);
		int idx = COLOR(pc) == WHITE ? s : (s ^ 56);
		int v = VALUE[t] + (t == KING && endgame ? KING_END[idx] : PST[t][idx]);
		score += COLOR(pc) == WHITE ? v : -v;
	}
	return p->side == WHITE ? score : -score;
}

/* ---- Search ------------------------------------------------------------------------------------ */

#define MATE 30000

typedef struct {
	Pos pos;
	u64 history[512];        /* keys of earlier positions, for repetition */
	int nhist;
	int max_depth;
	i64 deadline_ms;
	bool stop;
	u64 nodes;
	Move best;
} Search;

static int order_score(const Pos *p, Move m, Move hint)
{
	if (m.from == hint.from && m.to == hint.to && m.promo == hint.promo)
		return 1000000;
	int s = 0;
	if (m.flags & MF_CAPTURE) {
		int victim = (m.flags & MF_EP) ? PAWN : TYPE(p->sq[m.to]);
		s += 10000 + VALUE[victim] * 10 - VALUE[TYPE(p->sq[m.from])] / 10;
	}
	if (m.promo)
		s += 8000 + VALUE[m.promo];
	return s;
}

static void sort_moves(const Pos *p, Move *list, int n, Move hint)
{
	int score[256];
	for (int i = 0; i < n; i++)
		score[i] = order_score(p, list[i], hint);
	for (int i = 1; i < n; i++) {
		Move m = list[i];
		int sc = score[i], j = i;
		while (j > 0 && score[j - 1] < sc) {
			list[j] = list[j - 1];
			score[j] = score[j - 1];
			j--;
		}
		list[j] = m;
		score[j] = sc;
	}
}

static bool time_up(Search *s)
{
	if ((s->nodes & 2047) == 0 && s->deadline_ms && (i64)time_mono_ms() > s->deadline_ms)
		s->stop = true;
	return s->stop;
}

static int quiesce(Search *s, int alpha, int beta, int depth)
{
	s->nodes++;
	if (time_up(s))
		return 0;
	int stand = evaluate(&s->pos);
	if (stand >= beta)
		return beta;
	if (stand > alpha)
		alpha = stand;
	if (depth > 8)
		return alpha;
	Move list[256];
	int n = gen_moves(&s->pos, list, true);
	sort_moves(&s->pos, list, n, (Move){ 0 });
	for (int i = 0; i < n; i++) {
		Undo u;
		make_move(&s->pos, list[i], &u);
		if (in_check(&s->pos, s->pos.side ^ 8)) {
			unmake_move(&s->pos, &u);
			continue;
		}
		int sc = -quiesce(s, -beta, -alpha, depth + 1);
		unmake_move(&s->pos, &u);
		if (sc >= beta)
			return beta;
		if (sc > alpha)
			alpha = sc;
	}
	return alpha;
}

static bool repeated(Search *s)
{
	for (int i = s->nhist - 2; i >= 0 && i >= s->nhist - 1 - s->pos.halfmove; i -= 2)
		if (s->history[i] == s->pos.key)
			return true;
	return false;
}

static int search(Search *s, int depth, int alpha, int beta, int ply, Move *best_out, Move hint)
{
	s->nodes++;
	if (time_up(s))
		return 0;
	if (ply > 0 && (repeated(s) || s->pos.halfmove >= 100))
		return 0;
	bool check = in_check(&s->pos, s->pos.side);
	if (check)
		depth++;   /* look further when in check */
	if (depth <= 0)
		return quiesce(s, alpha, beta, 0);
	Move list[256];
	int n = gen_moves(&s->pos, list, false);
	sort_moves(&s->pos, list, n, hint);
	int legal = 0;
	Move best = { 0 };
	for (int i = 0; i < n; i++) {
		Undo u;
		make_move(&s->pos, list[i], &u);
		if (in_check(&s->pos, s->pos.side ^ 8)) {
			unmake_move(&s->pos, &u);
			continue;
		}
		legal++;
		if (s->nhist < (int)ARRAY_LEN(s->history))
			s->history[s->nhist++] = s->pos.key;
		int sc = -search(s, depth - 1, -beta, -alpha, ply + 1, NULL, (Move){ 0 });
		s->nhist--;
		unmake_move(&s->pos, &u);
		if (s->stop)
			return 0;
		if (sc > alpha) {
			alpha = sc;
			best = list[i];
			if (sc >= beta)
				break;
		}
	}
	if (!legal)
		return check ? -MATE + ply : 0;
	if (best_out)
		*best_out = best.from || best.to ? best : list[0];
	return alpha;
}

/* ---- Game state ------------------------------------------------------------------------------- */

enum { LEVEL_EASY, LEVEL_MEDIUM, LEVEL_HARD, LEVEL_EXPERT };
enum { RESULT_NONE, RESULT_WHITE, RESULT_BLACK, RESULT_DRAW };

static struct {
	Pos pos;
	Undo undo[1024];
	char san[1024][12];
	u64 keys[1024];
	int nmoves;
	int you;                 /* WHITE or BLACK */
	int level;
	bool flipped;
	int selected;            /* square, or -1 */
	Move legal[256];
	int nlegal;
	int result;
	char result_text[64];
	/* the computer's turn */
	int thinking;            /* atomic: 1 while the search runs */
	Search search;
	Move reply;
	int reply_ready;
	Ui *ui;
	/* piece pictures */
	Canvas *art[16];
	int art_size;
	f64 moved_at;
	int anim_from, anim_to;
	Conf *conf;
} G;

static void san_of(Pos *p, Move m, char *out)
{
	static const char LETTER[7] = { 0, 0, 'N', 'B', 'R', 'Q', 'K' };
	int n = 0;
	u8 pc = p->sq[m.from];
	if (m.flags & MF_CASTLE) {
		str_copy(out, m.to > m.from ? "O-O" : "O-O-O", 8);
		n = (int)strlen(out);
	} else {
		if (TYPE(pc) == PAWN) {
			if (m.flags & MF_CAPTURE)
				out[n++] = (char)('a' + (m.from & 7));
		} else {
			out[n++] = LETTER[TYPE(pc)];
			/* disambiguate between two of the same piece */
			Move list[256];
			int k = legal_moves(p, list);
			bool same_file = false, same_rank = false, other = false;
			for (int i = 0; i < k; i++) {
				if (list[i].to != m.to || list[i].from == m.from || p->sq[list[i].from] != pc)
					continue;
				other = true;
				if ((list[i].from & 7) == (m.from & 7))
					same_file = true;
				if ((list[i].from >> 3) == (m.from >> 3))
					same_rank = true;
			}
			if (other) {
				if (!same_file)
					out[n++] = (char)('a' + (m.from & 7));
				else if (!same_rank)
					out[n++] = (char)('1' + (m.from >> 3));
				else {
					out[n++] = (char)('a' + (m.from & 7));
					out[n++] = (char)('1' + (m.from >> 3));
				}
			}
		}
		if (m.flags & MF_CAPTURE)
			out[n++] = 'x';
		out[n++] = (char)('a' + (m.to & 7));
		out[n++] = (char)('1' + (m.to >> 3));
		if (m.promo) {
			out[n++] = '=';
			out[n++] = LETTER[m.promo];
		}
	}
	Undo u;
	make_move(p, m, &u);
	if (in_check(p, p->side)) {
		Move list[256];
		out[n++] = legal_moves(p, list) ? '+' : '#';
	}
	unmake_move(p, &u);
	out[n] = 0;
}

static bool insufficient(const Pos *p)
{
	int minors = 0;
	for (int s = 0; s < 64; s++) {
		int t = TYPE(p->sq[s]);
		if (t == PAWN || t == ROOK || t == QUEEN)
			return false;
		if (t == KNIGHT || t == BISHOP)
			minors++;
	}
	return minors <= 1;
}

static void check_result(void)
{
	G.nlegal = legal_moves(&G.pos, G.legal);
	G.result = RESULT_NONE;
	if (!G.nlegal) {
		if (in_check(&G.pos, G.pos.side)) {
			G.result = G.pos.side == WHITE ? RESULT_BLACK : RESULT_WHITE;
			str_copy(G.result_text, "Checkmate", sizeof G.result_text);
		} else {
			G.result = RESULT_DRAW;
			str_copy(G.result_text, "Stalemate", sizeof G.result_text);
		}
	} else if (G.pos.halfmove >= 100) {
		G.result = RESULT_DRAW;
		str_copy(G.result_text, "Draw by the fifty-move rule", sizeof G.result_text);
	} else if (insufficient(&G.pos)) {
		G.result = RESULT_DRAW;
		str_copy(G.result_text, "Draw: too little material to mate", sizeof G.result_text);
	} else {
		int same = 0;
		for (int i = 0; i < G.nmoves; i++)
			same += G.keys[i] == G.pos.key;
		if (same >= 2) {
			G.result = RESULT_DRAW;
			str_copy(G.result_text, "Draw by repetition", sizeof G.result_text);
		}
	}
	if (G.result != RESULT_NONE)
		sound_play(G.result == RESULT_DRAW ? "complete" : (G.result == RESULT_WHITE) == (G.you == WHITE) ? "win" : "lose");
}

static void play(Move m)
{
	if (G.nmoves >= 1023)
		return;
	san_of(&G.pos, m, G.san[G.nmoves]);
	G.keys[G.nmoves] = G.pos.key;
	bool capture = m.flags & MF_CAPTURE;
	make_move(&G.pos, m, &G.undo[G.nmoves]);
	G.nmoves++;
	G.selected = -1;
	G.anim_from = m.from, G.anim_to = m.to;
	G.moved_at = G.ui ? G.ui->now : 0;
	sound_play(capture ? "hit" : "click");
	check_result();
}

static void think(void)
{
	Search *s = &G.search;
	s->stop = false;
	s->nodes = 0;
	Move best = { 0 };
	Move list[256];
	int n = legal_moves(&s->pos, list);
	if (n)
		best = list[0];
	for (int d = 1; d <= s->max_depth && !s->stop; d++) {
		Move m = { 0 };
		search(s, d, -MATE - 1, MATE + 1, 0, &m, best);
		if (!s->stop && (m.from || m.to))
			best = m;
	}
	/* Easy plays a reasonable move, not always the best one. */
	if (G.level == LEVEL_EASY && n > 1 && rand_range(0, 99) < 35)
		best = list[rand_range(0, (u32)n - 1)];
	G.reply = best;
	atomic_store(&G.reply_ready, 1);
	atomic_store(&G.thinking, 0);
	futex_wake(&G.thinking, 1);
	ui_wake(G.ui);
}

/* One thread for the engine, woken for each move. */
static int think_requests;

static void engine_thread(void *arg)
{
	UNUSED(arg);
	int seen = 0;
	for (;;) {
		int now = atomic_load(&think_requests);
		if (now == seen) {
			futex_wait(&think_requests, now, -1);
			continue;
		}
		seen = now;
		think();
	}
}

static void computer_move(void)
{
	if (atomic_load(&G.thinking) || G.result || G.pos.side == G.you)
		return;
	static const int depth[4] = { 1, 2, 4, 6 };
	static const int ms[4] = { 300, 800, 2500, 5000 };
	Search *s = &G.search;
	s->pos = G.pos;
	s->nhist = MIN(G.nmoves, (int)ARRAY_LEN(s->history));
	for (int i = 0; i < s->nhist; i++)
		s->history[i] = G.keys[G.nmoves - s->nhist + i];
	s->max_depth = depth[G.level];
	s->deadline_ms = (i64)time_mono_ms() + ms[G.level];
	atomic_store(&G.thinking, 1);
	atomic_add(&think_requests, 1);
	futex_wake(&think_requests, 1);
}

static void new_game(int you)
{
	if (atomic_load(&G.thinking)) {
		G.search.stop = true;
		while (atomic_load(&G.thinking))
			futex_wait(&G.thinking, 1, 10);
	}
	pos_start(&G.pos);
	G.nmoves = 0;
	G.you = you;
	G.flipped = you == BLACK;
	G.selected = -1;
	G.reply_ready = 0;
	G.anim_from = G.anim_to = -1;
	check_result();
	computer_move();
}

static void take_back(void)
{
	if (atomic_load(&G.thinking))
		return;
	/* back to the last position where it was your turn */
	int n = 0;
	while (G.nmoves > 0 && (n == 0 || G.pos.side != G.you)) {
		G.nmoves--;
		unmake_move(&G.pos, &G.undo[G.nmoves]);
		n++;
	}
	G.selected = -1;
	G.anim_from = G.anim_to = -1;
	check_result();
	computer_move();
}

/* ---- Piece art (drawn for Zurbax) ------------------------------------------------------------------ */

static const char *piece_svg(int type)
{
	switch (type) {
	case PAWN:
		return "<path d='M22.5 9 a5 5 0 0 1 3.2 8.8 c2.6 1.5 4.1 4 4.1 7 0 2.2-.8 4-2.2 5.2 L31 36 H14 l3.4-6 c-1.4-1.2-2.2-3-2.2-5.2 0-3 1.5-5.5 4.1-7 A5 5 0 0 1 22.5 9 z'/>"
		       "<path d='M11 39 h23 l-1.5-3.5 h-20 z'/>";
	case ROOK:
		return "<path d='M11 39 h23 l-1.4-4 H12.4 z'/><path d='M14 35 l1.6-17 h13.8 L31 35 z'/>"
		       "<path d='M12.5 18.5 h20 l1-4 V9 h-4 v3 h-3.5 V9 h-5 v3 H17.5 V9 h-4 v5.5 z'/>";
	case KNIGHT:
		return "<path d='M14 38 h20 c0-6-1-12 1-18 1.8-5.4-.7-10.5-6.5-12.4 L27 5 l-2 3.4 c-6 .4-10.2 4.6-11.6 10 L10 25 l3 3 4-1.6 3.2-2.6 C19.4 28 15 31 14 38 z'/>"
		       "<circle cx='20' cy='14.5' r='1.3' fill='MARK' stroke='none'/><path d='M11 39 h23 l-1-3 H12 z'/>";
	case BISHOP:
		return "<path d='M12 39 h21 l-1.5-3.5 h-18 z'/><path d='M15 35.5 c1-3 1.5-5 1-7.5 C13.5 26 13 22.5 15 18.5 c1.8-3.4 4.5-6 7.5-8.2 3 2.2 5.7 4.8 7.5 8.2 2 4 1.5 7.5-1 9.5 -.5 2.5 0 4.5 1 7.5 z'/>"
		       "<circle cx='22.5' cy='7.5' r='2.4'/><path d='M22.5 17 v6 M19.5 20 h6' fill='none' stroke='MARK'/>";
	case QUEEN:
		return "<path d='M11 39 h23 l-1.5-4 h-20 z'/><path d='M13 35 L10 15 l6 8 2.5-11 4 10 4-10 2.5 11 6-8 -3 20 z'/>"
		       "<circle cx='10' cy='13' r='2.2'/><circle cx='18.5' cy='10' r='2.2'/><circle cx='26.5' cy='10' r='2.2'/><circle cx='35' cy='13' r='2.2'/><circle cx='22.5' cy='8.5' r='2.2'/>";
	case KING:
		return "<path d='M22.5 4 v7 M19 7.5 h7' fill='none'/><path d='M11 39 h23 l-1.5-4 h-20 z'/>"
		       "<path d='M13 35 c-1-3-4.5-7-4.5-11.5 0-4 3-6.5 6.5-6.5 3 0 5.5 2 7.5 4.5 2-2.5 4.5-4.5 7.5-4.5 3.5 0 6.5 2.5 6.5 6.5 0 4.5-3.5 8.5-4.5 11.5 z'/>"
		       "<path d='M22.5 21 c-1.2-3-1.2-5.5 0-8 1.2 2.5 1.2 5 0 8 z'/>";
	}
	return "";
}

static void load_art(int size)
{
	if (G.art_size == size)
		return;
	for (int i = 0; i < 16; i++) {
		canvas_free(G.art[i]);
		G.art[i] = NULL;
	}
	for (int c = 0; c < 2; c++)
		for (int t = PAWN; t <= KING; t++) {
			const char *fill = c == 0 ? "#fbf8f1" : "#2f2a26", *stroke = c == 0 ? "#26211d" : "#0f0d0b", *mark = c == 0 ? "#26211d" : "#e9e2d4";
			char *body = str_replace(piece_svg(t), "MARK", mark);
			char *svg = str_fmt("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 45 45'><g fill='%s' stroke='%s' stroke-width='1.5' "
					    "stroke-linejoin='round' stroke-linecap='round'>%s</g></svg>", fill, stroke, body);
			SvgDoc *d = svg_parse(svg, strlen(svg));
			if (d) {
				G.art[t | (c ? BLACK : WHITE)] = svg_rasterize(d, size, size, COLOR_BLACK);
				svg_free(d);
			}
			mem_free(svg);
			mem_free(body);
		}
	G.art_size = size;
}

/* ---- Drawing ------------------------------------------------------------------------------------------ */

static Rect square_rect(Rect board, int s)
{
	f32 cell = board.w / 8;
	int f = s & 7, r = s >> 3;
	if (G.flipped)
		f = 7 - f, r = 7 - r;
	return rect(board.x + cell * (f32)f, board.y + cell * (f32)(7 - r), cell, cell);
}

static int square_at(Rect board, f32 x, f32 y)
{
	if (!rect_contains(board, x, y))
		return -1;
	f32 cell = board.w / 8;
	int f = (int)((x - board.x) / cell), r = 7 - (int)((y - board.y) / cell);
	if (G.flipped)
		f = 7 - f, r = 7 - r;
	return r * 8 + f;
}

static void draw_board(Ui *ui, Rect board)
{
	Theme *t = &ui->theme;
	f32 cell = board.w / 8;
	ui_shadow(ui, board, 6, 18, 6, 0x50000000);
	Color light = 0xffe9d6b6, dark = 0xffb48a62;
	for (int s = 0; s < 64; s++) {
		Rect r = square_rect(board, s);
		bool is_light = ((s & 7) + (s >> 3)) % 2 == 1;
		ui_fill(ui, r, is_light ? light : dark);
	}
	ui_border(ui, rect_inset(board, -1, -1), 3, 2, 0xff5a3f2a);
	/* last move */
	if (G.nmoves) {
		Move m = G.undo[G.nmoves - 1].m;
		ui_fill(ui, square_rect(board, m.from), 0x55f6d34a);
		ui_fill(ui, square_rect(board, m.to), 0x70f6d34a);
	}
	/* check */
	if (in_check(&G.pos, G.pos.side)) {
		Rect k = square_rect(board, king_square(&G.pos, G.pos.side));
		ui_circle(ui, k.x + k.w / 2, k.y + k.h / 2, k.w * 0.48f, 0x70e01b24);
	}
	if (G.selected >= 0)
		ui_fill(ui, square_rect(board, G.selected), color_with_alpha(t->accent, 0x70));
	/* coordinates */
	Font cf = ui_font(ui, maxf(9, cell * 0.16f), 700);
	for (int i = 0; i < 8; i++) {
		int f = G.flipped ? 7 - i : i;
		int r = G.flipped ? i : 7 - i;
		char a[2] = { (char)('a' + f), 0 }, n[2] = { (char)('1' + r), 0 };
		/* each label takes the other square color, so it reads */
		int bottom_rank = G.flipped ? 7 : 0, left_file = G.flipped ? 7 : 0;
		bool file_light = (f + bottom_rank) % 2 == 1, rank_light = (left_file + r) % 2 == 1;
		ui_text(ui, cf, board.x + cell * (f32)i + cell - cf.size * 0.75f, board.y + board.h - 3, a, file_light ? dark : light);
		ui_text(ui, cf, board.x + 3, board.y + cell * (f32)i + cf.size + 2, n, rank_light ? dark : light);
	}
	/* pieces, with the last move sliding in */
	int px = (int)roundf(cell * ui->scale * 0.92f);
	load_art(px);
	f32 k = G.anim_to >= 0 ? clampf((f32)(ui->now - G.moved_at) / 0.16f, 0, 1) : 1;
	if (k < 1)
		ui->redraw = true;
	for (int s = 0; s < 64; s++) {
		u8 pc = G.pos.sq[s];
		if (!pc || !G.art[pc])
			continue;
		Rect r = square_rect(board, s);
		if (s == G.anim_to && k < 1) {
			Rect from = square_rect(board, G.anim_from);
			f32 e = ease_out_cubic(k);
			r.x = lerpf(from.x, r.x, e), r.y = lerpf(from.y, r.y, e);
		}
		ui_image(ui, rect_inset(r, cell * 0.04f, cell * 0.04f), G.art[pc], 0, false);
	}
	/* where the selected piece can go */
	if (G.selected >= 0)
		for (int i = 0; i < G.nlegal; i++) {
			if (G.legal[i].from != G.selected)
				continue;
			Rect r = square_rect(board, G.legal[i].to);
			if (G.pos.sq[G.legal[i].to])
				ui_ring(ui, r.x + r.w / 2, r.y + r.h / 2, r.w * 0.44f, cell * 0.07f, 0x60202020);
			else
				ui_circle(ui, r.x + r.w / 2, r.y + r.h / 2, r.w * 0.15f, 0x50202020);
		}
}

static void board_input(Ui *ui, Rect board)
{
	UiInteract in = ui_interact(ui, ui_id(ui, "board"), board);
	if (!in.pressed_now || G.result || G.pos.side != G.you || atomic_load(&G.thinking))
		return;
	int s = square_at(board, ui->mx, ui->my);
	if (s < 0)
		return;
	if (G.selected >= 0) {
		for (int i = 0; i < G.nlegal; i++) {
			Move m = G.legal[i];
			if (m.from == G.selected && m.to == s && (!m.promo || m.promo == QUEEN)) {
				play(m);
				computer_move();
				return;
			}
		}
	}
	u8 pc = G.pos.sq[s];
	G.selected = pc && COLOR(pc) == G.you && s != G.selected ? s : -1;
}

static void side_panel(Ui *ui, Rect r)
{
	Theme *t = &ui->theme;
	ui_card(ui, r);
	Rect c = pad(r, 16);
	/* players */
	static const char *const LEVELS[] = { "Easy", "Medium", "Hard", "Expert" };
	for (int i = 0; i < 2; i++) {
		int side = i == 0 ? (G.you ^ 8) : G.you;
		Rect row = cut_top(&c, 44);
		bool to_move = G.pos.side == side && !G.result;
		ui_round(ui, rect(row.x, row.y + 8, 28, 28), 8, side == WHITE ? 0xfffbf8f1 : 0xff2f2a26);
		ui_border(ui, rect(row.x, row.y + 8, 28, 28), 8, 1, t->border_strong);
		char who[64];
		if (side == G.you)
			str_copy(who, ui_user_name(ui) && *ui_user_name(ui) ? ui_user_name(ui) : "You", sizeof who);
		else
			fmt(who, sizeof who, "Computer · %s", LEVELS[G.level]);
		ui_text_in(ui, ui_font(ui, 14, to_move ? 700 : 500), rect(row.x + 40, row.y + 4, row.w - 70, 36), who, t->text, ALIGN_START);
		if (to_move && side != G.you && atomic_load(&G.thinking))
			ui_spinner(ui, rect(row.x + row.w - 22, row.y + 14, 18, 18));
		else if (to_move)
			ui_circle(ui, row.x + row.w - 12, row.y + 22, 5, t->accent);
		if (i == 0)
			cut_top(&c, 4);
	}
	cut_top(&c, 10);
	ui_separator(ui, c.x, c.y, c.x + c.w);
	cut_top(&c, 10);
	/* controls at the bottom */
	Rect bottom = cut_bottom(&c, 132);
	Rect lv = cut_top(&bottom, 32);
	int level = G.level;
	if (ui_segmented(ui, lv, LEVELS, 4, &level) && level != G.level) {
		G.level = level;
		conf_set_int(G.conf, "level", level);
		conf_save(G.conf);
	}
	cut_top(&bottom, 10);
	Rect row = cut_top(&bottom, 36);
	f32 bw = (row.w - 8) / 2;
	if (ui_button_suggested(ui, cut_left(&row, bw), "New Game"))
		ui_popover_toggle(ui, "newgame");
	cut_left(&row, 8);
	if (ui_button(ui, row, "Take Back"))
		take_back();
	cut_top(&bottom, 8);
	row = cut_top(&bottom, 36);
	if (ui_button_icon_label(ui, row, "refresh", "Turn Board"))
		G.flipped = !G.flipped;
	Rect anchor = rect(r.x + 16, r.y + r.h - 132 + 42, bw, 36);
	Rect pc;
	if (ui_popover_begin(ui, "newgame", anchor, 220, 100, &pc)) {
		if (ui_menu_item(ui, cut_top(&pc, 40), NULL, "Play White", NULL)) {
			new_game(WHITE);
			ui_popover_close(ui);
		}
		if (ui_menu_item(ui, cut_top(&pc, 40), NULL, "Play Black", NULL)) {
			new_game(BLACK);
			ui_popover_close(ui);
		}
		ui_popover_end(ui);
	}
	/* moves */
	ui_text(ui, ui_font(ui, 12, 700), c.x, c.y + 12, "MOVES", t->text_dim);
	cut_top(&c, 22);
	f32 y = ui_scroll_begin(ui, c, "moves");
	Font mf = ui_font(ui, 13, 500);
	int rows = (G.nmoves + 1) / 2;
	for (int i = 0; i < rows; i++) {
		f32 ry = y + 24 * (f32)i;
		if (i % 2 == 1)
			ui_round(ui, rect(c.x, ry, c.w, 24), 5, t->hover);
		char num[8];
		fmt(num, sizeof num, "%d.", i + 1);
		ui_text(ui, mf, c.x + 6, ry + 17, num, t->text_faint);
		ui_text(ui, ui_font(ui, 13, i * 2 == G.nmoves - 1 ? 700 : 500), c.x + 44, ry + 17, G.san[i * 2], t->text);
		if (i * 2 + 1 < G.nmoves)
			ui_text(ui, ui_font(ui, 13, i * 2 + 1 == G.nmoves - 1 ? 700 : 500), c.x + 44 + (c.w - 44) / 2, ry + 17, G.san[i * 2 + 1], t->text);
	}
	ui_scroll_end(ui, 24 * (f32)rows + 8);
	static int last_rows;
	if (rows != last_rows) {
		ui_scroll_to(ui, "moves", maxf(0, 24 * (f32)rows - c.h + 8));
		last_rows = rows;
	}
}

static void frame(Ui *ui)
{
	Theme *t = &ui->theme;
	G.ui = ui;
	if (atomic_load(&G.reply_ready)) {
		atomic_store(&G.reply_ready, 0);
		if (!G.result && G.pos.side != G.you)
			play(G.reply);
	}
	ui_window_background(ui, t->window);
	Rect win = rect(0, 0, ui->width, ui->height);
	ui_gradient(ui, win, 0, color_mix(t->window, 0xff6b4a30, t->dark ? 0.25f : 0.12f), t->window);
	Rect header = cut_top(&win, 46);
	ui_headerbar(ui, header, "Chess", HEADER_TRANSPARENT | HEADER_FLAT);
	win = pad(win, 20);
	Rect panel = cut_right(&win, minf(300, win.w * 0.36f));
	cut_right(&win, 20);
	f32 size = floorf(minf(win.w, win.h) / 8) * 8;
	Rect board = rect(win.x + (win.w - size) / 2, win.y + (win.h - size) / 2, size, size);
	board_input(ui, board);
	draw_board(ui, board);
	side_panel(ui, panel);
	if (G.result) {
		Rect card = center_in(board, minf(300, size - 40), 150);
		ui_shadow(ui, card, 14, 24, 8, 0x60000000);
		ui_round(ui, card, 14, t->card);
		bool won = G.result != RESULT_DRAW && (G.result == RESULT_WHITE) == (G.you == WHITE);
		const char *title = G.result == RESULT_DRAW ? "Draw" : won ? "You won!" : "The computer won";
		ui_text_in(ui, ui_font(ui, 20, 700), rect(card.x, card.y + 18, card.w, 30), title, t->text, ALIGN_CENTER);
		ui_text_in(ui, ui_font(ui, 13, 500), rect(card.x, card.y + 50, card.w, 22), G.result_text, t->text_dim, ALIGN_CENTER);
		if (ui_button_suggested(ui, rect(card.x + (card.w - 140) / 2, card.y + 92, 140, 36), "Play Again"))
			new_game(G.you);
	}
	if (ui_key_pressed(ui, KEY_Z, MOD_CTRL))
		take_back();
	if (ui_key_pressed(ui, KEY_N, MOD_CTRL))
		new_game(G.you);
	if (ui_key_pressed(ui, KEY_ESC, 0))
		G.selected = -1;
}

static void init(Ui *ui)
{
	G.ui = ui;
	zobrist_init();
	char *path = path_join(home_dir(), ".config/zurbax/chess.conf");
	G.conf = conf_load(path);
	mem_free(path);
	G.level = (int)CLAMP(conf_get_int(G.conf, "level", LEVEL_MEDIUM), LEVEL_EASY, LEVEL_EXPERT);
	thread_spawn(engine_thread, NULL);
	new_game(WHITE);
}

int main(void)
{
	UiAppDesc d = { .id = "chess", .title = "Chess", .width = 1000, .height = 720, .min_width = 700, .min_height = 520,
			.flags = WIN_RESIZABLE, .init = init, .frame = frame };
	return ui_run(&d);
}
