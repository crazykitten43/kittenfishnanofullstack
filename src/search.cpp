#include "micro.hpp"
#include "weights.hpp"
#include <algorithm>
#include <cstdlib>

namespace micro {
namespace {
constexpr int Inf = Mate+1;
constexpr int Upper=1, Lower=2, Exact=3;
int toTT(int score,int ply) {
    return score > Mate-MaxPly ? score+ply : score < -Mate+MaxPly ? score-ply : score;
}
int fromTT(int score,int ply) {
    return score > Mate-MaxPly ? score-ply : score < -Mate+MaxPly ? score+ply : score;
}
bool hasLegal(Board& b, Moves& moves) {
    for (int i=0;i<moves.size;++i) {
        Undo u;
        if (!b.make(moves.list[i],u)) continue;
        b.unmake(moves.list[i],u);
        return true;
    }
    return false;
}
}

void Search::clear() {
    if (table_ && entries_)
        for (size_t i=0;i<entries_;++i) table_[i].bound=0;
}

bool Search::stop() {
    if ((limits_.nodes && nodes_>=limits_.nodes) ||
        (deadline_ && (nodes_&63)==0 && clock_()>=deadline_))
        stopped_=true;
    return stopped_;
}

Result Search::run(Board& b, Limits limits, const uint64_t* previous, size_t count) {
    limits_=limits;
    deadline_=clock_ && limits.milliseconds>0 ? clock_()+uint64_t(limits.milliseconds)*1000 : 0;
    previous_=previous;
    previousCount_=previous ? count : 0;
    nodes_=0;
    stopped_=false;
    history_[0]=0;
    for (size_t i=0;i<previousCount_ && i<b.halfmove;++i)
        history_[0]+=previous_[previousCount_-1-i];
    for (auto& row : killers_) row[0]=row[1]=Move{};
    Result result{};
    auto& moves=frames_[0].moves;
    b.generate(moves);
    for (int i=0;i<moves.size;++i) {
        Undo u;
        Move m=moves.list[i];
        if (!b.make(m,u)) continue;
        b.unmake(m,u);
        result.move=m;
        break;
    }
    constexpr int aspirationWindow=40;
    for (int d=1;d<=std::min(limits.depth,MaxPly-2);++d) {
        root_=Move{};
        int lower=-Inf, upper=Inf;
        if (profile_==SearchProfile::Aspiration && d>=3 && std::abs(result.score)<Mate-MaxPly) {
            lower=result.score-aspirationWindow;
            upper=result.score+aspirationWindow;
        }
        int score=negamax(b,d,0,lower,upper);
        if (!stopped_ && (score<=lower || score>=upper)) {
            root_=Move{};
            score=negamax(b,d,0,-Inf,Inf);
        }
        if (stopped_) break;
        if (root_) result.move=root_;
        result.score=score;
        result.depth=d;
        if (!root_ || score>Mate-MaxPly || score<-Mate+MaxPly ||
            (deadline_ && clock_()>=deadline_)) break;
    }
    result.nodes=nodes_;
    return result;
}

int Search::negamax(Board& b,int depth,int ply,int alpha,int beta) {
    ++nodes_;
    if (stop()) return 0;
    const bool check=b.check();
    const bool q=depth<=0;
    keys_[ply]=b.key;
    int repeats=0;
    for (int distance=2;distance<=b.halfmove;distance+=2) {
        int at=ply-distance;
        size_t back=at<0 ? size_t(-at) : 0;
        if (back>previousCount_) break;
        if ((at>=0 ? keys_[at] : previous_[previousCount_-back])==b.key) ++repeats;
        if (repeats==2) break;
    }
    auto& frame=frames_[ply];
    auto& moves=frame.moves;
    if (b.halfmove>=100 || repeats>=2 || b.dead() || ply==MaxPly-1) {
        b.generate(moves);
        if (!hasLegal(b,moves)) return check ? -Mate+ply : 0;
        return ply==MaxPly-1 && b.halfmove<100 && repeats<2 && !b.dead() ?
            eval_.score(b,eval_.context) : 0;
    }
    // Include the reversible history to avoid using a bound from a context
    // with different draw rights. Commutative sum permits equivalent histories.
    uint64_t ttKey=b.key ^ (history_[ply]*0x9e3779b97f4a7c15ULL) ^
                   (uint64_t(b.halfmove)*0xbf58476d1ce4e5b9ULL);
    TTEntry* slot=!q && table_ && entries_ ? &table_[tableIndex(ttKey,entries_)] : nullptr;
    Move ttMove;
    if (slot && slot->bound && slot->key==ttKey) {
        ttMove.code=slot->move;
        int score=fromTT(slot->score,ply);
        if (ply && slot->depth>=depth &&
            (slot->bound==Exact || (slot->bound==Lower && score>=beta) ||
             (slot->bound==Upper && score<=alpha))) return score;
    }
    int start=alpha, best=-Inf;
    if (q && !check) {
        best=eval_.score(b,eval_.context);
        alpha=std::max(best,alpha);
    }
    b.generate(moves,q && !check);
    for (int i=0;i<moves.size;++i) {
        Move m=moves.list[i];
        int victim=type(b.square[m.to()]), attacker=type(b.square[m.from()]);
        if (!victim && attacker==Pawn && m.to()==b.ep) victim=Pawn;
        int order=0;
        if (m.code==ttMove.code) order=30000;
        else if (victim || m.promotion())
            order=10000+8*weights::material[victim]-weights::material[attacker]+
                  3*weights::material[m.promotion()];
        else if (m.code==killers_[ply][0].code) order=9000;
        else if (m.code==killers_[ply][1].code) order=8900;
        frame.order[i]=int16_t(order);
    }
    Move bestMove;
    int legal=0;
    for (int i=0;i<moves.size;++i) {
        int pick=i;
        for (int j=i+1;j<moves.size;++j)
            if (frame.order[j]>frame.order[pick]) pick=j;
        std::swap(moves.list[i],moves.list[pick]);
        std::swap(frame.order[i],frame.order[pick]);
        Move m=moves.list[i];
        Undo u;
        if (!b.make(m,u)) continue;
        ++legal;
        if (q && !check && best>=beta) {
            b.unmake(m,u);
            return best;
        }
        history_[ply+1]=b.halfmove ? history_[ply]+u.key : 0;
        if (eval_.push) eval_.push(b,m,u,eval_.context);
        int score;
        // Reduce only late quiet moves at non-PV nodes. Re-search at
        // nominal depth whenever the reduced search improves alpha.
        const bool reduce=profile_==SearchProfile::Lmr && ply && depth>=3 &&
            beta-alpha==1 && legal>=4 && !check && !u.captured &&
            !m.promotion() && m.code!=ttMove.code &&
            m.code!=killers_[ply][0].code && m.code!=killers_[ply][1].code && !b.check();
        if (reduce) {
            score=-negamax(b,depth-2,ply+1,-beta,-alpha);
            if (score>alpha && !stopped_)
                score=-negamax(b,depth-1,ply+1,-beta,-alpha);
        } else if (profile_==SearchProfile::AlphaBeta || legal==1 || q) score=-negamax(b,depth-1,ply+1,-beta,-alpha);
        else {
            score=-negamax(b,depth-1,ply+1,-alpha-1,-alpha);
            if (score>alpha && score<beta && !stopped_)
                score=-negamax(b,depth-1,ply+1,-beta,-alpha);
        }
        if (eval_.pop) eval_.pop(b,m,u,eval_.context);
        b.unmake(m,u);
        if (stopped_) return 0;
        if (score>best) {
            best=score;
            bestMove=m;
            if (!ply) root_=m;
        }
        alpha=std::max(alpha,score);
        if (alpha>=beta) {
            if (!q && !u.captured && !m.promotion() && m.code!=killers_[ply][0].code) {
                killers_[ply][1]=killers_[ply][0];
                killers_[ply][0]=m;
            }
            break;
        }
    }
    if (!legal) {
        if (check) return -Mate+ply;
        if (!q) return 0;
        b.generate(moves);
        if (!hasLegal(b,moves)) return 0;
    }
    if (slot && (!slot->bound || slot->key!=ttKey || slot->depth<=depth || best>start))
        *slot={ttKey,bestMove.code,int16_t(toTT(best,ply)),int8_t(depth),
               uint8_t(best>=beta ? Lower : best<=start ? Upper : Exact)};
    return best;
}
}
