#include "micro.hpp"
#include "weights.hpp"
#include "geometry.hpp"
#include <algorithm>
#include <cstdlib>
#include <initializer_list>

namespace micro {
namespace {
using namespace weights;
constexpr uint64_t fileA = 0x0101010101010101ULL;
constexpr uint64_t fileH = fileA << 7;
int count(uint64_t bits) { return __builtin_popcountll(bits); }
uint64_t bit(int sq) { return uint64_t(1) << index64(sq); }
using geometry::take;
uint64_t pawnAttacks(uint64_t p, int side) {
    if (side == White) return ((p & ~fileA)<<7) | ((p & ~fileH)<<9);
    return ((p & ~fileA)>>9) | ((p & ~fileH)>>7);
}
uint64_t adjacentFiles(int f) {
    return (f ? fileA << (f-1) : 0) | (f<7 ? fileA << (f+1) : 0);
}
uint64_t ahead(int sq, int c) {
    int rank = sq>>4;
    return c == White ? (~uint64_t(0) << ((rank+1)*8)) :
                        ((uint64_t(1) << (rank*8))-1);
}
int pawnScore(uint64_t own, uint64_t enemy, int c, uint64_t supports) {
    int score=0;
    for (int f=0;f<8;++f) {
        uint64_t onFile=own & (fileA<<f), adjacent=adjacentFiles(f);
        if (!onFile) continue;
        score -= (count(onFile)-1)*doubled;
        if (!(own&adjacent)) score -= count(onFile)*isolated;
        while (onFile) {
            int s=take(onFile), r=s>>4, relative=c==White ? r : 7-r;
            uint64_t sq=bit(s);
            bool supported=supports&sq;
            if (supported || (own & adjacent & (uint64_t(255)<<(8*r))))
                score += connected;
            // A doubled pawn behind its partner receives no passer bonus.
            uint64_t front=ahead(s,c);
            if (!(front & ((enemy & (adjacent|(fileA<<f))) | (own & (fileA<<f)))))
                score += passed[relative] + (supported ? protectedPasser : 0);
        }
    }
    return score;
}
uint64_t destinations(const Board& b,int sq,int piece) {
    uint64_t targets=0;
    geometry::attacks(b,sq,piece,[&](int to) { targets |= bit(to); });
    return targets;
}
}

int Hce::call(const Board& board, void* context) {
    return static_cast<Hce*>(context)->evaluate(board);
}

int Hce::evaluate(const Board& b) const {
    using namespace weights;
    const int stage=std::min(b.phase,maxPhase);
    int middle=b.mg, end=b.eg;
    uint64_t pawn[2]={b.pieces[White][Pawn],b.pieces[Black][Pawn]};
    uint64_t pawnMap[2]={pawnAttacks(pawn[White],White),pawnAttacks(pawn[Black],Black)};
    {
        PawnEntry* slot = cache && capacity ? &cache[tableIndex(b.pawnKey,capacity)] : nullptr;
        int value;
        if (slot && slot->valid && slot->key==b.pawnKey) value=slot->score;
        else {
            value=pawnScore(pawn[White],pawn[Black],White,pawnMap[White]) -
                  pawnScore(pawn[Black],pawn[White],Black,pawnMap[Black]);
            if (slot) *slot={b.pawnKey,value,true};
        }
        middle+=value/2;
        end+=value;
    }
    uint64_t ring[2]={0,0};
    for (int c : {White,Black})
        ring[c]=destinations(b,b.king[c],sign(c)*King);
    for (int c : {White,Black}) {
        int enemy=c^1, activity=0, danger=0;
        uint64_t own=0;
        for (int t=Pawn;t<=King;++t) own |= b.pieces[c][t];
        if (count(b.pieces[c][Bishop]) >= 2)
            activity+=bishopPair;
        danger=count(pawnMap[c]&ring[enemy]);
        for (int t=Knight;t<=Queen;++t) {
            uint64_t pieces=b.pieces[c][t];
            while (pieces) {
                int sq=take(pieces), f=sq&7, r=sq>>4, relative=c==White ? r : 7-r;
                uint64_t targets=destinations(b,sq,sign(c)*t);
                danger += count(targets&ring[enemy])*ringWeight[t];
                activity += count(targets & ~own & ~pawnMap[enemy])*mobility[t];
                if (t==Knight && relative>=3 && relative<=5 && (pawnMap[c]&bit(sq)) &&
                    !(pawn[enemy]&adjacentFiles(f)&ahead(sq,c))) activity+=outpost;
                if (t==Rook) {
                    uint64_t file=fileA<<f;
                    if (!(file&pawn[c])) activity += file&pawn[enemy] ? semiOpenRook : openRook;
                    int enemyRank=c==White ? b.king[enemy]>>4 : 7-(b.king[enemy]>>4);
                    if (relative==6 && enemyRank>=6) activity+=seventh;
                }
            }
        }
        middle+=sign(c)*activity;
        end+=sign(c)*activity*3/4;
        int safety=0, king=b.king[c], forward=16*sign(c);
        for (int f=std::max(0,(king&7)-1);f<=std::min(7,(king&7)+1);++f) {
            int near=(king&0x70)+f+forward, far=near+forward;
            if (valid(near) && b.square[near]==sign(c)*Pawn) safety+=shelterNear;
            else if (valid(far) && b.square[far]==sign(c)*Pawn) safety+=shelterFar;
            uint64_t file=fileA<<f;
            if (!(file&pawn[c])) safety -= file&pawn[enemy] ? kingSemiOpen : kingOpen;
        }
        middle+=sign(c)*safety;
        danger=std::min(danger,dangerCap);
        middle+=sign(c)*danger*danger/dangerDivisor;
    }
    return sign(b.side)*(middle*stage+end*(maxPhase-stage))/maxPhase;
}
}
