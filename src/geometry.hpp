#pragma once
#include "micro.hpp"
#include <initializer_list>

namespace micro { namespace geometry {
constexpr int knight[] = {33,31,18,14,-14,-18,-31,-33};
constexpr int rays[] = {15,17,-15,-17,1,-1,16,-16};

inline int take(uint64_t& bits) {
    int i=__builtin_ctzll(bits);
    bits &= bits-1;
    return (i&7) + 16*(i>>3);
}

template <class Visit>
void attacks(const Board& b, int sq, int piece, Visit visit) {
    int t=type(piece);
    if (t==Pawn) {
        for (int d : {15,17}) {
            int to=sq+d*sign(color(piece));
            if (valid(to)) visit(to);
        }
        return;
    }
    if (t==Knight) {
        for (int d : knight) if (valid(sq+d)) visit(sq+d);
        return;
    }
    for (int i=(t==Rook ? 4 : 0); i<(t==Bishop ? 4 : 8); ++i) {
        int d=rays[i];
        for (int to=sq+d; valid(to); to+=d) {
            visit(to);
            if (t==King || b.square[to]) break;
        }
    }
}
}}
