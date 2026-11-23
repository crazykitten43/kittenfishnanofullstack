#include "micro.hpp"
#include "weights.hpp"
#include "geometry.hpp"
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace micro {
namespace {
using namespace geometry;
constexpr int castleMask(int sq) {
    return sq == 4 ? 12 : sq == 116 ? 3 : sq == 0 ? 13 :
           sq == 7 ? 14 : sq == 112 ? 7 : sq == 119 ? 11 : 15;
}
constexpr uint64_t mix(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
struct Zobrist {
    uint64_t piece[14][64]{}, castle[16]{}, ep[9]{}, turn = mix(1000);
    constexpr Zobrist() {
        for (int p=0; p<14; ++p)
            for (int s=0; s<64; ++s) piece[p][s] = mix(p*64+s);
        for (int i=0; i<16; ++i) castle[i] = mix(900+i);
        for (int i=0; i<9; ++i) ep[i] = mix(950+i);
    }
};
constexpr Zobrist zobrist;
constexpr char symbols[] = " PNBRQK";
int readSquare(const char* p) {
    if (p[0] < 'a' || p[0] > 'h' || p[1] < '1' || p[1] > '8') return -1;
    return (p[0]-'a') + 16*(p[1]-'1');
}
}

void Board::put(int sq, int p) {
    int old = square[sq], i = index64(sq);
    if (old == p) return;
    uint64_t bit = uint64_t(1) << i;
    for (int piece : {old,p}) {
        if (!piece) continue;
        int c = color(piece), t = type(piece);
        int delta = (piece == old ? -1 : 1) * sign(c);
        uint64_t z = zobrist.piece[c*7+t][i];
        pieces[c][t] ^= bit;
        key ^= z;
        if (t == Pawn) pawnKey ^= z;
        if (t == King && piece != old) king[c] = sq;
        int pos = i ^ (c == White ? 56 : 0);
        material += delta * weights::material[t];
        mg += delta * (weights::mgValue[t] + weights::mgPst[t][pos]);
        eg += delta * (weights::egValue[t] + weights::egPst[t][pos]);
        phase += (piece == old ? -1 : 1) * weights::phase[t];
    }
    square[sq] = int8_t(p);
}

bool Board::load(const char* fen) {
    Board parsed;
    if (!fen || !parsed.parse(fen)) return false;
    *this = parsed;
    return true;
}

bool Board::parse(const char* fen) {
    int sq = 112, files = 0;
    for (; *fen && *fen != ' '; ++fen) {
        char ch = *fen;
        if (ch == '/') {
            if (files != 8 || (sq >> 4) == 0) return false;
            sq -= 16;
            files = 0;
        } else if (ch >= '1' && ch <= '8') {
            files += ch-'0';
            if (files > 8) return false;
        } else {
            bool lower = ch >= 'a' && ch <= 'z';
            const char* p = std::strchr(symbols+1, lower ? ch-'a'+'A' : ch);
            if (!p || files >= 8) return false;
            int t = int(p-symbols);
            if (t == Pawn && (sq == 0 || sq == 112)) return false;
            put(sq+files++, lower ? -t : t);
        }
    }
    if (sq || files != 8 || *fen++ != ' ') return false;
    for (int c : {White,Black}) {
        int total=0, extra=0;
        for (int t=Pawn; t<=King; ++t) {
            int count=__builtin_popcountll(pieces[c][t]);
            total+=count;
            int initial=t==Queen ? 1 : 2;
            if (t>=Knight && t<=Queen && count>initial) extra+=count-initial;
        }
        if (__builtin_popcountll(pieces[c][King])!=1 || total>16 ||
            extra+__builtin_popcountll(pieces[c][Pawn])>8) return false;
    }
    if (*fen != 'w' && *fen != 'b') return false;
    side = *fen++ == 'w' ? White : Black;
    if (*fen++ != ' ') return false;
    if (*fen == '-') ++fen;
    else {
        constexpr char castling[] = "KQkq";
        while (*fen && *fen != ' ') {
            const char* p = std::strchr(castling, *fen++);
            if (!p) return false;
            rights |= 1 << (p-castling);
        }
        if (!rights) return false;
    }
    for (int c : {White,Black}) {
        int home=c==White ? 4 : 116, mask=(rights>>(2*c))&3;
        if ((mask && king[c]!=home) ||
            ((mask&1) && square[home+3]!=sign(c)*Rook) ||
            ((mask&2) && square[home-4]!=sign(c)*Rook)) return false;
    }
    if (*fen++ != ' ') return false;
    if (*fen == '-') ++fen;
    else {
        ep = readSquare(fen);
        if (ep < 0 || (ep>>4) != (side == White ? 5 : 2) || square[ep] ||
            square[ep-16*sign(side)] != -sign(side)*Pawn ||
            square[ep+16*sign(side)]) return false;
        fen += 2;
    }
    if (*fen == ' ') {
        char* end;
        const char* clock = fen+1;
        if (*clock < '0' || *clock > '9') return false;
        unsigned long n = std::strtoul(clock, &end, 10);
        if (n > 65535 || (*end && *end != ' ')) return false;
        halfmove = uint16_t(n);
        if (*end) {
            const char* number=end+1;
            if (*number<'1' || *number>'9') return false;
            std::strtoul(number,&end,10);
            if (*end) return false;
        }
    } else if (*fen) return false;
    if (attacked(king[side^1], side)) return false;
    key ^= zobrist.castle[rights] ^ zobrist.ep[epFile()+1];
    if (side == Black) key ^= zobrist.turn;
    return true;
}

bool Board::attacked(int sq, int by) const {
    int s = sign(by);
    for (int offset : {15,17}) {
        int a = sq - offset*s;
        if (valid(a) && square[a] == s*Pawn) return true;
    }
    for (int offset : knight) {
        int a = sq + offset;
        if (valid(a) && square[a] == s*Knight) return true;
    }
    for (int i=0; i<8; ++i) {
        int d = rays[i], a = sq+d;
        if (valid(a) && square[a] == s*King) return true;
        for (; valid(a); a+=d) {
            int p = square[a];
            if (!p) continue;
            if (p == s*Queen || p == s*(i < 4 ? Bishop : Rook)) return true;
            break;
        }
    }
    return false;
}

int Board::epFile() const {
    if (ep < 0) return -1;
    int behind = ep - 16*sign(side);
    for (int from : {behind-1,behind+1}) {
        if (!valid(from) || square[from] != sign(side)*Pawn) continue;
        Board trial = *this;
        trial.square[from] = trial.square[behind] = 0;
        trial.square[ep] = int8_t(sign(side)*Pawn);
        if (!trial.check()) return ep & 7;
    }
    return -1;
}

void Board::generate(Moves& moves, bool noisy) const {
    moves.size = 0;
    int direction = sign(side)*16;
    uint64_t occupied=0;
    for (int t=Pawn;t<=King;++t) occupied |= pieces[side][t];
    while (occupied) {
        int from=take(occupied), p=square[from], rank=from>>4;
        if (type(p) == Pawn) {
            bool promotes = rank == (side == White ? 6 : 1);
            auto add = [&](int to) {
                if (promotes) {
                    for (int t=Queen;t>=Knight;--t) moves.add(Move(from,to,t));
                } else moves.add(Move(from,to));
            };
            int front = from+direction;
            if ((!noisy || promotes) && !square[front]) {
                add(front);
                if (rank == (side == White ? 1 : 6) && !square[front+direction])
                    add(front+direction);
            }
            for (int to : {front-1,front+1}) {
                if (!valid(to)) continue;
                int target = square[to];
                if (to == ep || (target && color(target)!=side && type(target)!=King))
                    add(to);
            }
            continue;
        }
        attacks(*this,from,p,[&](int to) {
            int target=square[to];
            if ((!target && !noisy) ||
                (target && color(target)!=side && type(target)!=King))
                moves.add(Move(from,to));
        });
    }
    if (noisy) return;
    int home = side == White ? 4 : 116, mask = rights >> (2*side);
    if ((mask & 3) == 0 || king[side] != home || check()) return;
    int rook = sign(side)*Rook, enemy = side^1;
    if ((mask&1) && square[home+3]==rook && !square[home+1] && !square[home+2] &&
        !attacked(home+1,enemy)) moves.add(Move(home,home+2));
    if ((mask&2) && square[home-4]==rook && !square[home-1] && !square[home-2] &&
        !square[home-3] && !attacked(home-1,enemy)) moves.add(Move(home,home-2));
}

bool Board::make(Move m, Undo& u) {
    int from=m.from(), to=m.to(), p=square[from], t=type(p), step=16*sign(side);
    int captured = t==Pawn && to==ep ? to-step : to;
    u = {key,halfmove,uint8_t(rights),int8_t(ep),square[captured]};
    key ^= zobrist.castle[rights] ^ zobrist.ep[epFile()+1] ^ zobrist.turn;
    ep = t==Pawn && to-from==2*step ? from+step : -1;
    rights &= castleMask(from) & castleMask(to);
    halfmove = t==Pawn || u.captured ? 0 : uint16_t(halfmove + (halfmove < 65535));
    put(from,0);
    if (u.captured) put(captured,0);
    put(to,m.promotion() ? sign(side)*m.promotion() : p);
    if (t==King && (to-from==2 || to-from==-2)) {
        int rook = to>from ? from+3 : from-4;
        put(rook,0);
        put((from+to)/2,sign(side)*Rook);
    }
    int moving = side;
    side ^= 1;
    if (attacked(king[moving],side)) {
        unmake(m,u);
        return false;
    }
    key ^= zobrist.castle[rights] ^ zobrist.ep[epFile()+1];
    return true;
}

void Board::unmake(Move m, const Undo& u) {
    int from=m.from(), to=m.to();
    side ^= 1;
    int p = m.promotion() ? sign(side)*Pawn : square[to];
    if (type(p)==King && (to-from==2 || to-from==-2)) {
        put((from+to)/2,0);
        put(to>from ? from+3 : from-4,sign(side)*Rook);
    }
    put(to,0);
    put(from,p);
    int captured = type(p)==Pawn && to==u.ep ? to-16*sign(side) : to;
    if (u.captured) put(captured,u.captured);
    key=u.key; rights=u.rights; ep=u.ep; halfmove=u.halfmove;
}

bool Board::dead() const {
    for (int side : {White,Black})
        if (pieces[side][Pawn] || pieces[side][Rook] || pieces[side][Queen]) return false;
    uint64_t n = pieces[White][Knight] | pieces[Black][Knight];
    uint64_t b = pieces[White][Bishop] | pieces[Black][Bishop];
    constexpr uint64_t dark = 0xaa55aa55aa55aa55ULL;
    return __builtin_popcountll(n|b) <= 1 || (!n && (!(b&dark) || !(b&~dark)));
}

void notation(Move m, char out[6]) {
    if (!m) { std::memcpy(out,"0000",5); return; }
    out[0]=char('a'+(m.from()&7)); out[1]=char('1'+(m.from()>>4));
    out[2]=char('a'+(m.to()&7)); out[3]=char('1'+(m.to()>>4));
    out[4]=m.promotion() ? char(symbols[m.promotion()]+'a'-'A') : 0;
    out[5]=0;
}

Move parseMove(Board& b, const char* text) {
    Moves moves;
    b.generate(moves);
    for (int i=0;i<moves.size;++i) {
        char name[6];
        notation(moves.list[i],name);
        if (std::strcmp(name,text)) continue;
        Undo u;
        if (b.make(moves.list[i],u)) {
            b.unmake(moves.list[i],u);
            return moves.list[i];
        }
    }
    return {};
}

uint64_t perft(Board& b, int depth) {
    if (depth <= 0) return 1;
    Moves moves;
    b.generate(moves);
    uint64_t total=0;
    for (int i=0;i<moves.size;++i) {
        Undo u;
        if (!b.make(moves.list[i],u)) continue;
        total += perft(b,depth-1);
        b.unmake(moves.list[i],u);
    }
    return total;
}
}
