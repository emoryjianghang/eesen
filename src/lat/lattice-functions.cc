// lat/lattice-functions.cc

// Copyright 2009-2011  Saarland University (Author: Arnab Ghoshal)
//           2012-2013  Johns Hopkins University (Author: Daniel Povey);  Chao Weng;
//                      Bagher BabaAli
//                2013  Cisco Systems (author: Neha Agrawal) [code modified
//                      from original code in ../gmmbin/gmm-rescore-lattice.cc]
//                2014  Guoguo Chen

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.


#include "lat/lattice-functions.h"
//#include "hmm/transition-model.h"
#include "util/stl-utils.h"
#include "base/kaldi-math.h"
//#include "hmm/hmm-utils.h"

namespace eesen {
using std::map;
using std::vector;

int32 LatticeStateTimes(const Lattice &lat, vector<int32> *times) {
  if (!lat.Properties(fst::kTopSorted, true))
    KALDI_ERR << "Input lattice must be topologically sorted.";
  KALDI_ASSERT(lat.Start() == 0);
  int32 num_states = lat.NumStates();
  times->clear();
  times->resize(num_states, -1);
  (*times)[0] = 0;
  for (int32 state = 0; state < num_states; state++) { 
    int32 cur_time = (*times)[state];
    for (fst::ArcIterator<Lattice> aiter(lat, state); !aiter.Done();
        aiter.Next()) {
      const LatticeArc &arc = aiter.Value();

      if (arc.ilabel != 0) {  // Non-epsilon input label on arc
        // next time instance
        if ((*times)[arc.nextstate] == -1) {
          (*times)[arc.nextstate] = cur_time + 1;
        } else {
          KALDI_ASSERT((*times)[arc.nextstate] == cur_time + 1);
        }
      } else {  // epsilon input label on arc
        // Same time instance
        if ((*times)[arc.nextstate] == -1)
          (*times)[arc.nextstate] = cur_time;
        else
          KALDI_ASSERT((*times)[arc.nextstate] == cur_time);
      }
    }
  }
  return (*std::max_element(times->begin(), times->end()));
}

int32 CompactLatticeStateTimes(const CompactLattice &lat, vector<int32> *times) {
  if (!lat.Properties(fst::kTopSorted, true))
    KALDI_ERR << "Input lattice must be topologically sorted.";
  KALDI_ASSERT(lat.Start() == 0);
  int32 num_states = lat.NumStates();
  times->clear();
  times->resize(num_states, -1);
  (*times)[0] = 0;
  int32 utt_len = -1;
  for (int32 state = 0; state < num_states; state++) {
    int32 cur_time = (*times)[state];
    for (fst::ArcIterator<CompactLattice> aiter(lat, state); !aiter.Done();
        aiter.Next()) {
      const CompactLatticeArc &arc = aiter.Value();
      int32 arc_len = static_cast<int32>(arc.weight.String().size());
      if ((*times)[arc.nextstate] == -1)
        (*times)[arc.nextstate] = cur_time + arc_len;
      else
        KALDI_ASSERT((*times)[arc.nextstate] == cur_time + arc_len);
    }
    if (lat.Final(state) != CompactLatticeWeight::Zero()) {
      int32 this_utt_len = (*times)[state] + lat.Final(state).String().size();
      if (utt_len == -1) utt_len = this_utt_len;
      else {
        if (this_utt_len != utt_len) {
          KALDI_WARN << "Utterance does not "
              "seem to have a consistent length.";
          utt_len = std::max(utt_len, this_utt_len);
        }
      }
    }        
  }
  if (utt_len == -1) {
    KALDI_WARN << "Utterance does not have a final-state.";
    return 0;
  }
  return utt_len;
}

template<class LatType> // could be Lattice or CompactLattice
bool PruneLattice(BaseFloat beam, LatType *lat) {
  typedef typename LatType::Arc Arc;
  typedef typename Arc::Weight Weight;
  typedef typename Arc::StateId StateId;
  
  KALDI_ASSERT(beam > 0.0);
  if (!lat->Properties(fst::kTopSorted, true)) {
    if (fst::TopSort(lat) == false) {
      KALDI_WARN << "Cycles detected in lattice";
      return false;
    }
  }
  // We assume states before "start" are not reachable, since
  // the lattice is topologically sorted.
  int32 start = lat->Start();
  int32 num_states = lat->NumStates();
  if (num_states == 0) return false;
  std::vector<double> forward_cost(num_states,
                                   std::numeric_limits<double>::infinity()); // viterbi forward.
  forward_cost[start] = 0.0; // lattice can't have cycles so couldn't be
  // less than this.
  double best_final_cost = std::numeric_limits<double>::infinity();
  // Update the forward probs.
  // Thanks to Jing Zheng for finding a bug here.
  for (int32 state = 0; state < num_states; state++) {
    double this_forward_cost = forward_cost[state];
    for (fst::ArcIterator<LatType> aiter(*lat, state);
         !aiter.Done();
         aiter.Next()) {
      const Arc &arc(aiter.Value());
      StateId nextstate = arc.nextstate;
      KALDI_ASSERT(nextstate > state && nextstate < num_states);
      double next_forward_cost = this_forward_cost +
          ConvertToCost(arc.weight);
      if (forward_cost[nextstate] > next_forward_cost)
        forward_cost[nextstate] = next_forward_cost;
    }
    Weight final_weight = lat->Final(state);
    double this_final_cost = this_forward_cost +
        ConvertToCost(final_weight);
    if (this_final_cost < best_final_cost)
      best_final_cost = this_final_cost;
  }
  int32 bad_state = lat->AddState(); // this state is not final.
  double cutoff = best_final_cost + beam;
  
  // Go backwards updating the backward probs (which share memory with the
  // forward probs), and pruning arcs and deleting final-probs.  We prune arcs
  // by making them point to the non-final state "bad_state".  We'll then use
  // Trim() to remove unnecessary arcs and states.  [this is just easier than
  // doing it ourselves.]
  std::vector<double> &backward_cost(forward_cost);
  for (int32 state = num_states - 1; state >= 0; state--) {
    double this_forward_cost = forward_cost[state];
    double this_backward_cost = ConvertToCost(lat->Final(state));
    if (this_backward_cost + this_forward_cost > cutoff
        && this_backward_cost != std::numeric_limits<double>::infinity())
      lat->SetFinal(state, Weight::Zero());
    for (fst::MutableArcIterator<LatType> aiter(lat, state);
         !aiter.Done();
         aiter.Next()) {
      Arc arc(aiter.Value());
      StateId nextstate = arc.nextstate;
      KALDI_ASSERT(nextstate > state && nextstate < num_states);
      double arc_cost = ConvertToCost(arc.weight),
          arc_backward_cost = arc_cost + backward_cost[nextstate],
          this_fb_cost = this_forward_cost + arc_backward_cost;
      if (arc_backward_cost < this_backward_cost)
        this_backward_cost = arc_backward_cost;
      if (this_fb_cost > cutoff) { // Prune the arc.
        arc.nextstate = bad_state;
        aiter.SetValue(arc);
      }
    }
    backward_cost[state] = this_backward_cost;
  }
  fst::Connect(lat);
  return (lat->NumStates() > 0);
}

// instantiate the template for lattice and CompactLattice.
template bool PruneLattice(BaseFloat beam, Lattice *lat);
template bool PruneLattice(BaseFloat beam, CompactLattice *lat);


static inline double LogAddOrMax(bool viterbi, double a, double b) {
  if (viterbi)
    return std::max(a, b);
  else
    return LogAdd(a, b);
}

// Computes (normal or Viterbi) alphas and betas; returns (total-prob, or
// best-path negated cost) Note: in either case, the alphas and betas are
// negated costs.  Requires that lat be topologically sorted.  This code
// will work for either CompactLattice or Latice.
template<typename LatticeType>
static double ComputeLatticeAlphasAndBetas(const LatticeType &lat,
                                           bool viterbi,
                                           vector<double> *alpha,
                                           vector<double> *beta) {
  typedef typename LatticeType::Arc Arc;
  typedef typename Arc::Weight Weight;
  typedef typename Arc::StateId StateId;

  StateId num_states = lat.NumStates();
  KALDI_ASSERT(lat.Properties(fst::kTopSorted, true) == fst::kTopSorted);
  KALDI_ASSERT(lat.Start() == 0);
  alpha->resize(num_states, kLogZeroDouble);
  beta->resize(num_states, kLogZeroDouble);

  double tot_forward_prob = kLogZeroDouble;
  (*alpha)[0] = 0.0;
  // Propagate alphas forward.
  for (StateId s = 0; s < num_states; s++) {
    double this_alpha = (*alpha)[s];
    for (fst::ArcIterator<LatticeType> aiter(lat, s); !aiter.Done();
         aiter.Next()) {
      const Arc &arc = aiter.Value();
      double arc_like = -ConvertToCost(arc.weight);
      (*alpha)[arc.nextstate] = LogAddOrMax(viterbi, (*alpha)[arc.nextstate],
                                                this_alpha + arc_like);
    }
    Weight f = lat.Final(s);
    if (f != Weight::Zero()) {
      double final_like = this_alpha - ConvertToCost(f);
      tot_forward_prob = LogAddOrMax(viterbi, tot_forward_prob, final_like);
    }
  }
  for (StateId s = num_states-1; s >= 0; s--) { // it's guaranteed signed.
    double this_beta = -ConvertToCost(lat.Final(s));
    for (fst::ArcIterator<LatticeType> aiter(lat, s); !aiter.Done();
         aiter.Next()) {
      const Arc &arc = aiter.Value();
      double arc_like = -ConvertToCost(arc.weight),
          arc_beta = (*beta)[arc.nextstate] + arc_like;
      this_beta = LogAddOrMax(viterbi, this_beta, arc_beta);
    }
    (*beta)[s] = this_beta;
  }
  double tot_backward_prob = (*beta)[lat.Start()];
  if (!ApproxEqual(tot_forward_prob, tot_backward_prob, 1e-8)) {
    KALDI_WARN << "Total forward probability over lattice = " << tot_forward_prob
               << ", while total backward probability = " << tot_backward_prob;
  }
  // Split the difference when returning... they should be the same.
  return 0.5 * (tot_backward_prob + tot_forward_prob);
}



/// This is used in CompactLatticeLimitDepth.
struct LatticeArcRecord {
  BaseFloat logprob; // logprob <= 0 is the best Viterbi logprob of this arc,
                     // minus the overall best-cost of the lattice.
  CompactLatticeArc::StateId state; // state in the lattice.
  size_t arc; // arc index within the state.
  bool operator < (const LatticeArcRecord &other) const {
    return logprob < other.logprob;
  }
};

void CompactLatticeLimitDepth(int32 max_depth_per_frame,
                              CompactLattice *clat) {
  typedef CompactLatticeArc Arc;
  typedef Arc::Weight Weight;
  typedef Arc::StateId StateId;

  if (clat->Start() == fst::kNoStateId) {
    KALDI_WARN << "Limiting depth of empty lattice.";
    return;
  }
  if (clat->Properties(fst::kTopSorted, true) == 0) {
    if (!TopSort(clat))
      KALDI_ERR << "Topological sorting of lattice failed.";
  }
  
  vector<int32> state_times;
  int32 T = CompactLatticeStateTimes(*clat, &state_times);

  // The alpha and beta quantities here are "viterbi" alphas and beta.
  std::vector<double> alpha;
  std::vector<double> beta;
  bool viterbi = true;
  double best_prob = ComputeLatticeAlphasAndBetas(*clat, viterbi,
                                                  &alpha, &beta);

  std::vector<std::vector<LatticeArcRecord> > arc_records(T);

  StateId num_states = clat->NumStates();
  for (StateId s = 0; s < num_states; s++) {
    for (fst::ArcIterator<CompactLattice> aiter(*clat, s); !aiter.Done();
         aiter.Next()) {
      const Arc &arc = aiter.Value();
      LatticeArcRecord arc_record;
      arc_record.state = s;
      arc_record.arc = aiter.Position();
      arc_record.logprob =
          (alpha[s] + beta[arc.nextstate] - ConvertToCost(arc.weight))
           - best_prob;
      KALDI_ASSERT(arc_record.logprob < 0.1); // Should be zero or negative.
      int32 num_frames = arc.weight.String().size(), start_t = state_times[s];
      for (int32 t = start_t; t < start_t + num_frames; t++) {
        KALDI_ASSERT(t < T);
        arc_records[t].push_back(arc_record);
      }
    }
  }
  StateId dead_state = clat->AddState(); // A non-coaccesible state which we use
                                         // to remove arcs (make them end
                                         // there).
  size_t max_depth = max_depth_per_frame;
  for (int32 t = 0; t < T; t++) {
    size_t size = arc_records[t].size();
    if (size > max_depth) {
      // we sort from worst to best, so we keep the later-numbered ones,
      // and delete the lower-numbered ones.
      size_t cutoff = size - max_depth;
      std::nth_element(arc_records[t].begin(),
                       arc_records[t].begin() + cutoff,
                       arc_records[t].end());
      for (size_t index = 0; index < cutoff; index++) {
        LatticeArcRecord record(arc_records[t][index]);
        fst::MutableArcIterator<CompactLattice> aiter(clat, record.state);
        aiter.Seek(record.arc);
        Arc arc = aiter.Value();
        if (arc.nextstate != dead_state) { // not already killed.
          arc.nextstate = dead_state;
          aiter.SetValue(arc);
        }
      }
    }
  }
  Connect(clat);
  TopSortCompactLatticeIfNeeded(clat);
}


void TopSortCompactLatticeIfNeeded(CompactLattice *clat) {
  if (clat->Properties(fst::kTopSorted, true) == 0) {
    if (fst::TopSort(clat) == false) {
      KALDI_ERR << "Topological sorting failed";
    }
  }
}

void TopSortLatticeIfNeeded(Lattice *lat) {
  if (lat->Properties(fst::kTopSorted, true) == 0) {
    if (fst::TopSort(lat) == false) {
      KALDI_ERR << "Topological sorting failed";
    }
  }
}


/// Returns the depth of the lattice, defined as the average number of
/// arcs crossing any given frame.  Returns 1 for empty lattices.
/// Requires that input is topologically sorted.
BaseFloat CompactLatticeDepth(const CompactLattice &clat,
                              int32 *num_frames) {
  typedef CompactLattice::Arc::StateId StateId;
  if (clat.Properties(fst::kTopSorted, true) == 0) {
    KALDI_ERR << "Lattice input to CompactLatticeDepth was not topologically "
              << "sorted.";
  }
  if (clat.Start() == fst::kNoStateId) {
    *num_frames = 0;
    return 1.0;
  }
  size_t num_arc_frames = 0;
  int32 t;
  {
    vector<int32> state_times;
    t = CompactLatticeStateTimes(clat, &state_times);
  }
  if (num_frames != NULL)
    *num_frames = t;
  for (StateId s = 0; s < clat.NumStates(); s++) {
    for (fst::ArcIterator<CompactLattice> aiter(clat, s); !aiter.Done();
         aiter.Next()) {
      const CompactLatticeArc &arc = aiter.Value();
      num_arc_frames += arc.weight.String().size();
    }
    num_arc_frames += clat.Final(s).String().size();
  }
  return num_arc_frames / static_cast<BaseFloat>(t);
}


void CompactLatticeDepthPerFrame(const CompactLattice &clat,
                                 std::vector<int32> *depth_per_frame) {
  typedef CompactLattice::Arc::StateId StateId;
  if (clat.Properties(fst::kTopSorted, true) == 0) {
    KALDI_ERR << "Lattice input to CompactLatticeDepthPerFrame was not "
              << "topologically sorted.";
  }
  if (clat.Start() == fst::kNoStateId) {
    depth_per_frame->clear();
    return;
  }
  vector<int32> state_times;
  int32 T = CompactLatticeStateTimes(clat, &state_times);

  depth_per_frame->clear();
  if (T <= 0) {
    return;
  } else {
    depth_per_frame->resize(T, 0);
    for (StateId s = 0; s < clat.NumStates(); s++) {
      int32 start_time = state_times[s];
      for (fst::ArcIterator<CompactLattice> aiter(clat, s); !aiter.Done();
           aiter.Next()) {
        const CompactLatticeArc &arc = aiter.Value();
        int32 len = arc.weight.String().size();
        for (int32 t = start_time; t < start_time + len; t++) {
          KALDI_ASSERT(t < T);
          (*depth_per_frame)[t]++;
        }
      }
      int32 final_len = clat.Final(s).String().size();
      for (int32 t = start_time; t < start_time + final_len; t++) {
        KALDI_ASSERT(t < T);
        (*depth_per_frame)[t]++;
      }
    }
  }
}


bool CompactLatticeToWordAlignment(const CompactLattice &clat,
                                   std::vector<int32> *words,
                                   std::vector<int32> *begin_times,
                                   std::vector<int32> *lengths) {
  words->clear();
  begin_times->clear();
  lengths->clear();
  typedef CompactLattice::Arc Arc;
  typedef Arc::Label Label;
  typedef CompactLattice::StateId StateId;
  typedef CompactLattice::Weight Weight;
  using namespace fst;
  StateId state = clat.Start();
  int32 cur_time = 0;
  if (state == kNoStateId) {
    KALDI_WARN << "Empty lattice.";
    return false;
  }
  while (1) {
    Weight final = clat.Final(state);
    size_t num_arcs = clat.NumArcs(state);
    if (final != Weight::Zero()) {
      if (num_arcs != 0) {
        KALDI_WARN << "Lattice is not linear.";
        return false;
      }
      if (! final.String().empty()) {
        KALDI_WARN << "Lattice has alignments on final-weight: probably "
            "was not word-aligned (alignments will be approximate)";
      }
      return true;
    } else {
      if (num_arcs != 1) {
        KALDI_WARN << "Lattice is not linear: num-arcs = " << num_arcs;
        return false;
      }
      fst::ArcIterator<CompactLattice> aiter(clat, state);
      const Arc &arc = aiter.Value();
      Label word_id = arc.ilabel; // Note: ilabel==olabel, since acceptor.
      // Also note: word_id may be zero; we output it anyway.
      int32 length = arc.weight.String().size();
      words->push_back(word_id);
      begin_times->push_back(cur_time);
      lengths->push_back(length);
      cur_time += length;
      state = arc.nextstate;
    }
  }
}

void CompactLatticeShortestPath(const CompactLattice &clat,
                                CompactLattice *shortest_path) {
  using namespace fst;
  if (clat.Properties(fst::kTopSorted, true) == 0) {
    CompactLattice clat_copy(clat);
    if (!TopSort(&clat_copy))
      KALDI_ERR << "Was not able to topologically sort lattice (cycles found?)";
    CompactLatticeShortestPath(clat_copy, shortest_path);
    return;
  }
  // Now we can assume it's topologically sorted.
  shortest_path->DeleteStates();
  if (clat.Start() == kNoStateId) return;
  KALDI_ASSERT(clat.Start() == 0); // since top-sorted.
  typedef CompactLatticeArc Arc;
  typedef Arc::StateId StateId;
  typedef CompactLatticeWeight Weight;
  vector<std::pair<double, StateId> > best_cost_and_pred(clat.NumStates() + 1);
  StateId superfinal = clat.NumStates();
  for (StateId s = 0; s <= clat.NumStates(); s++) {
    best_cost_and_pred[s].first = numeric_limits<double>::infinity();
    best_cost_and_pred[s].second = fst::kNoStateId;
  }
  best_cost_and_pred[0].first = 0;
  for (StateId s = 0; s < clat.NumStates(); s++) {
    double my_cost = best_cost_and_pred[s].first;
    for (ArcIterator<CompactLattice> aiter(clat, s);
         !aiter.Done();
         aiter.Next()) {
      const Arc &arc = aiter.Value();
      double arc_cost = ConvertToCost(arc.weight),
          next_cost = my_cost + arc_cost;
      if (next_cost < best_cost_and_pred[arc.nextstate].first) {
        best_cost_and_pred[arc.nextstate].first = next_cost;
        best_cost_and_pred[arc.nextstate].second = s;
      }
    }
    double final_cost = ConvertToCost(clat.Final(s)),
        tot_final = my_cost + final_cost;
    if (tot_final < best_cost_and_pred[superfinal].first) {
      best_cost_and_pred[superfinal].first = tot_final;
      best_cost_and_pred[superfinal].second = s;
    }
  }
  std::vector<StateId> states; // states on best path.
  StateId cur_state = superfinal;
  while (cur_state != 0) {
    StateId prev_state = best_cost_and_pred[cur_state].second;
    if (prev_state == kNoStateId) {
      KALDI_WARN << "Failure in best-path algorithm for lattice (infinite costs?)";
      return; // return empty best-path.
    }
    states.push_back(prev_state);
    KALDI_ASSERT(cur_state != prev_state && "Lattice with cycles");
    cur_state = prev_state;
  }
  std::reverse(states.begin(), states.end());
  for (size_t i = 0; i < states.size(); i++)
    shortest_path->AddState();
  for (StateId s = 0; static_cast<size_t>(s) < states.size(); s++) {
    if (s == 0) shortest_path->SetStart(s);
    if (static_cast<size_t>(s + 1) < states.size()) { // transition to next state.
      bool have_arc = false;
      Arc cur_arc;
      for (ArcIterator<CompactLattice> aiter(clat, states[s]);
           !aiter.Done();
           aiter.Next()) {
        const Arc &arc = aiter.Value();
        if (arc.nextstate == states[s+1]) {
          if (!have_arc ||
              ConvertToCost(arc.weight) < ConvertToCost(cur_arc.weight)) {
            cur_arc = arc;
            have_arc = true;
          }
        }
      }
      KALDI_ASSERT(have_arc && "Code error.");
      shortest_path->AddArc(s, Arc(cur_arc.ilabel, cur_arc.olabel,
                                   cur_arc.weight, s+1));
    } else { // final-prob.
      shortest_path->SetFinal(s, clat.Final(states[s]));
    }
  }
}

void AddWordInsPenToCompactLattice(BaseFloat word_ins_penalty, 
                                   CompactLattice *clat) {
  typedef CompactLatticeArc Arc;
  int32 num_states = clat->NumStates();

  //scan the lattice
  for (int32 state = 0; state < num_states; state++) {
    for (fst::MutableArcIterator<CompactLattice> aiter(clat, state);
         !aiter.Done(); aiter.Next()) {
      
      Arc arc(aiter.Value());
      
      if (arc.ilabel != 0) { // if there is a word on this arc
        LatticeWeight weight = arc.weight.Weight();
        // add word insertion penalty to lattice
        weight.SetValue1( weight.Value1() + word_ins_penalty);    
        arc.weight.SetWeight(weight);
        aiter.SetValue(arc);
      } 
    } // end looping over arcs
  }  // end looping over states  
} 

struct ClatRescoreTuple {
  ClatRescoreTuple(int32 state, int32 arc, int32 tid):
      state_id(state), arc_id(arc), tid(tid) { }
  int32 state_id;
  int32 arc_id;
  int32 tid;
};


//bool RescoreCompactLattice(DecodableInterface *decodable,
//                           CompactLattice *clat) {
//  return RescoreCompactLatticeInternal(NULL, 1.0, decodable, clat);
//}


bool RescoreLattice(DecodableInterface *decodable,
                    Lattice *lat) {
  if (lat->NumStates() == 0) {
    KALDI_WARN << "Rescoring empty lattice";
    return false;
  }
  if (!lat->Properties(fst::kTopSorted, true)) {  
    if (fst::TopSort(lat) == false) {
      KALDI_WARN << "Cycles detected in lattice.";
      return false;
    }
  }
  std::vector<int32> state_times;
  int32 utt_len = eesen::LatticeStateTimes(*lat, &state_times);
  
  std::vector<std::vector<int32> > time_to_state(utt_len );
  
  int32 num_states = lat->NumStates();
  KALDI_ASSERT(num_states == state_times.size());
  for (size_t state = 0; state < num_states; state++) {
    int32 t = state_times[state];
    // Don't check t >= 0 because non-accessible states could have t = -1.
    KALDI_ASSERT(t <= utt_len);  
    if (t >= 0 && t < utt_len)
      time_to_state[t].push_back(state);
  }

  for (int32 t = 0; t < utt_len; t++) {
    if ((t < utt_len - 1) && decodable->IsLastFrame(t)) {
      KALDI_WARN << "Features are too short for lattice: utt-len is "
                 << utt_len << ", " << t << " is last frame";
      return false;
    }
    for (size_t i = 0; i < time_to_state[t].size(); i++) {
      int32 state = time_to_state[t][i];
      for (fst::MutableArcIterator<Lattice> aiter(lat, state);
           !aiter.Done(); aiter.Next()) {
        LatticeArc arc = aiter.Value();
        if (arc.ilabel != 0) {
          int32 trans_id = arc.ilabel; // Note: it doesn't necessarily
          // have to be a transition-id, just whatever the Decodable
          // object is expecting, but it's normally a transition-id.

          BaseFloat log_like = decodable->LogLikelihood(t, trans_id);
          arc.weight.SetValue2(-log_like + arc.weight.Value2());
          aiter.SetValue(arc);
        }
      }
    }
  }
  return true;
}

int32 LongestSentenceLength(const Lattice &lat) {
  typedef Lattice::Arc Arc;
  typedef Arc::Label Label;
  typedef Arc::StateId StateId;
  
  if (lat.Properties(fst::kTopSorted, true) == 0) {
    Lattice lat_copy(lat);
    if (!TopSort(&lat_copy))
      KALDI_ERR << "Was not able to topologically sort lattice (cycles found?)";
    return LongestSentenceLength(lat_copy);
  }
  std::vector<int32> max_length(lat.NumStates(), 0);
  int32 lattice_max_length = 0;
  for (StateId s = 0; s < lat.NumStates(); s++) {
    int32 this_max_length = max_length[s];
    for (fst::ArcIterator<Lattice> aiter(lat, s); !aiter.Done(); aiter.Next()) {
      const Arc &arc = aiter.Value();
      bool arc_has_word = (arc.olabel != 0);
      StateId nextstate = arc.nextstate;
      KALDI_ASSERT(static_cast<size_t>(nextstate) < max_length.size());
      if (arc_has_word) {
        // A lattice should ideally not have cycles anyway; a cycle with a word
        // on is something very bad.
        KALDI_ASSERT(nextstate > s && "Lattice has cycles with words on.");
        max_length[nextstate] = std::max(max_length[nextstate],
                                         this_max_length + 1);
      } else {
        max_length[nextstate] = std::max(max_length[nextstate],
                                         this_max_length);
      }
    }
    if (lat.Final(s) != LatticeWeight::Zero())
      lattice_max_length = std::max(lattice_max_length, max_length[s]);
  }
  return lattice_max_length;
}

int32 LongestSentenceLength(const CompactLattice &clat) {
  typedef CompactLattice::Arc Arc;
  typedef Arc::Label Label;
  typedef Arc::StateId StateId;
  
  if (clat.Properties(fst::kTopSorted, true) == 0) {
    CompactLattice clat_copy(clat);
    if (!TopSort(&clat_copy))
      KALDI_ERR << "Was not able to topologically sort lattice (cycles found?)";
    return LongestSentenceLength(clat_copy);
  }
  std::vector<int32> max_length(clat.NumStates(), 0);
  int32 lattice_max_length = 0;
  for (StateId s = 0; s < clat.NumStates(); s++) {
    int32 this_max_length = max_length[s];
    for (fst::ArcIterator<CompactLattice> aiter(clat, s);
         !aiter.Done(); aiter.Next()) {
      const Arc &arc = aiter.Value();
      bool arc_has_word = (arc.ilabel != 0); // note: olabel == ilabel.
      // also note: for normal CompactLattice, e.g. as produced by
      // determinization, all arcs will have nonzero labels, but the user might
      // decide to remplace some of the labels with zero for some reason, and we
      // want to support this.
      StateId nextstate = arc.nextstate;
      KALDI_ASSERT(static_cast<size_t>(nextstate) < max_length.size());
      KALDI_ASSERT(nextstate > s && "CompactLattice has cycles");
      if (arc_has_word)
        max_length[nextstate] = std::max(max_length[nextstate],
                                         this_max_length + 1);
      else
        max_length[nextstate] = std::max(max_length[nextstate],
                                         this_max_length);
    }
    if (clat.Final(s) != CompactLatticeWeight::Zero())
      lattice_max_length = std::max(lattice_max_length, max_length[s]);
  }
  return lattice_max_length;
}

void ComposeCompactLatticeDeterministic(
    const CompactLattice& clat,
    fst::DeterministicOnDemandFst<fst::StdArc>* det_fst,
    CompactLattice* composed_clat) {
  // StdFst::Arc and CompactLatticeArc has the same StateId type.
  typedef fst::StdArc::StateId StateId;
  typedef fst::StdArc::Weight Weight1;
  typedef CompactLatticeArc::Weight Weight2;
  typedef std::pair<StateId, StateId> StatePair;
  typedef unordered_map<StatePair, StateId, PairHasher<StateId> > MapType;
  typedef MapType::iterator IterType;

  // Empties the output FST.
  KALDI_ASSERT(composed_clat != NULL);
  composed_clat->DeleteStates();

  MapType state_map;
  std::queue<StatePair> state_queue;

  // Sets start state in <composed_clat>.
  StateId start_state = composed_clat->AddState();
  StatePair start_pair(clat.Start(), det_fst->Start());
  composed_clat->SetStart(start_state);
  state_queue.push(start_pair);
  std::pair<IterType, bool> result =
      state_map.insert(std::make_pair(start_pair, start_state));
  KALDI_ASSERT(result.second == true);

  // Starts composition here.
  while (!state_queue.empty()) {
    // Gets the first state in the queue.
    StatePair s = state_queue.front();
    StateId s1 = s.first;
    StateId s2 = s.second;
    state_queue.pop();

    // If the product of the final weights of the two states is not zero, then
    // we should create final state in fst_composed. We compute the product
    // manually since this is more efficient.
    Weight2 final_weight(LatticeWeight(clat.Final(s1).Weight().Value1() +
                                       det_fst->Final(s2).Value(),
                                       clat.Final(s1).Weight().Value2()),
                         clat.Final(s1).String());
    if (final_weight != Weight2::Zero()) {
      KALDI_ASSERT(state_map.find(s) != state_map.end());
      composed_clat->SetFinal(state_map[s], final_weight);
    }

    // Loops over pair of edges at s1 and s2.
    for (fst::ArcIterator<CompactLattice> aiter(clat, s1);
         !aiter.Done(); aiter.Next()) {
      const CompactLatticeArc& arc1 = aiter.Value();
      fst::StdArc arc2;
      StateId next_state1 = arc1.nextstate, next_state2;
      bool matched = false;

      if (arc1.olabel == 0) {
        // If the symbol on <arc1> is <epsilon>, we transit to the next state
        // for <clat>, but keep <det_fst> at the current state.
        matched = true;
        next_state2 = s2;
      } else {
        // Otherwise try to find the matched arc in <det_fst>.
        matched = det_fst->GetArc(s2, arc1.olabel, &arc2);
        if (matched) {
          next_state2 = arc2.nextstate;
        }
      }

      // If matched arc is found in <det_fst>, then we have to add new arcs to
      // <composed_clat>.
      if (matched) {
        StatePair next_state_pair(next_state1, next_state2);
        IterType siter = state_map.find(next_state_pair);
        StateId next_state;

        // Adds composed state to <state_map>.
        if (siter == state_map.end()) {
          // If the composed state has not been created yet, create it.
          next_state = composed_clat->AddState();
          std::pair<const StatePair, StateId> next_state_map(next_state_pair,
                                                             next_state);
          std::pair<IterType, bool> result = state_map.insert(next_state_map);
          KALDI_ASSERT(result.second);
          state_queue.push(next_state_pair);
        } else {
          // If the combposed state is already in <state_map>, we can directly
          // use that.
          next_state = siter->second;
        }

        // Adds arc to <composed_clat>.
        if (arc1.olabel == 0) {
          composed_clat->AddArc(state_map[s],
                                CompactLatticeArc(0, 0,
                                                  arc1.weight, next_state));
        } else {
          Weight2 composed_weight(
              LatticeWeight(arc1.weight.Weight().Value1() +
                            arc2.weight.Value(),
                            arc1.weight.Weight().Value2()),
              arc1.weight.String());
          composed_clat->AddArc(state_map[s],
                                CompactLatticeArc(arc1.ilabel, arc1.olabel,
                                                  composed_weight, next_state));
        }
      }
    }
  }
  fst::Connect(composed_clat);
}

}  // namespace eesen
