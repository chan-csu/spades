//***************************************************************************
//* Copyright (c) 2011-2013 Saint-Petersburg Academic University
//* All Rights Reserved
//* See file LICENSE for details.
//****************************************************************************

/*
 * graph_simplification.hpp
 *
 *  Created on: Aug 12, 2011
 *      Author: sergey
 */

#pragma once

#include "standard_base.hpp"
#include "config_struct.hpp"
#include "debruijn_graph.hpp"
#include "stats/debruijn_stats.hpp"

#include "omni/visualization/graph_colorer.hpp"
#include "omni/omni_utils.hpp"
#include "omni/omni_tools.hpp"
#include "omni/tip_clipper.hpp"
#include "omni/bulge_remover.hpp"
#include "omni/complex_bulge_remover.hpp"
#include "omni/erroneous_connection_remover.hpp"
#include "omni/relative_coverage_remover.hpp"
#include "omni/mf_ec_remover.hpp"
#include "utils.hpp"
#include "simplification/simplification_settings.hpp"
#include "simplification/parallel_simplification_algorithms.hpp"

#include "detail_coverage.hpp"
#include "graph_read_correction.hpp"
#include "detail_coverage.hpp"

#include "stats/chimera_stats.hpp"

namespace debruijn {

namespace simplification {

//todo remove this line
using namespace debruijn_graph;

//todo move to visualization
template<class graph_pack>
shared_ptr<omnigraph::visualization::GraphColorer<typename graph_pack::graph_t>> DefaultGPColorer(
        const graph_pack& gp) {
    auto mapper = MapperInstance(gp);
    auto path1 = mapper->MapSequence(gp.genome).path();
    auto path2 = mapper->MapSequence(!gp.genome).path();
    return omnigraph::visualization::DefaultColorer(gp.g, path1, path2);
}

template<class Graph>
class EditDistanceTrackingCallback {
    typedef typename Graph::EdgeId EdgeId;
    typedef typename Graph::EdgeData EdgeData;
    const Graph& g_;

public:
    EditDistanceTrackingCallback(const Graph& g)
            : g_(g) {
    }

    bool operator()(EdgeId edge, const vector<EdgeId>& path) const {
        vector<Sequence> path_sequences;
        for (auto it = path.begin(); it != path.end(); ++it) {
            path_sequences.push_back(g_.EdgeNucls(*it));
        }
        Sequence path_sequence(
            MergeOverlappingSequences(path_sequences, g_.k()));
        size_t dist = EditDistance(g_.EdgeNucls(edge), path_sequence);
        TRACE( "Bulge sequences with distance " << dist << " were " << g_.EdgeNucls(edge) << " and " << path_sequence);
        return true;
    }

private:
    DECL_LOGGER("EditDistanceTrackingCallback")
    ;
};

//enabling tip projection, todo optimize if hotspot
template<class gp_t>
boost::function<void(typename Graph::EdgeId)> WrapWithProjectionCallback(
    gp_t& gp,
    boost::function<void(typename Graph::EdgeId)> removal_handler_f) {
    typedef typename Graph::EdgeId EdgeId;
    typedef boost::function<void(EdgeId)> HandlerF;
    TipsProjector<gp_t> tip_projector(gp);

    HandlerF projecting_callback = boost::bind(&TipsProjector<gp_t>::ProjectTip,
                                               tip_projector, _1);

    return boost::bind(func::Composition<EdgeId>, _1,
                       boost::ref(removal_handler_f), projecting_callback);
}

template<class Graph>
bool ClipTips(
    Graph& g,
    const debruijn_config::simplification::tip_clipper& tc_config,
    const SimplifInfoContainer& info,
    boost::function<void(typename Graph::EdgeId)> removal_handler_f =
    0) {

    INFO("Clipping tips");

    string condition_str = tc_config.condition;
    ConditionParser<Graph> parser(g, condition_str, info);
    auto condition = parser();
    
    return ClipTips(g, parser.max_length_bound(), condition, removal_handler_f);
}

template<class gp_t>
bool ClipTips(
    gp_t& gp,
    const debruijn_config::simplification::tip_clipper& tc_config,
    const SimplifInfoContainer& info,
    bool enable_projection = false,
    boost::function<void(typename gp_t::graph_t::EdgeId)> removal_handler_f =
    0) {

    if (enable_projection)
        removal_handler_f = WrapWithProjectionCallback(gp, removal_handler_f);  

    return ClipTips(gp.g, tc_config, info, removal_handler_f);
}

template<class Graph>
bool RemoveBulges(
    Graph& g,
    const debruijn_config::simplification::bulge_remover& br_config,
    boost::function<void(EdgeId, const std::vector<EdgeId> &)> opt_handler = 0,
    boost::function<void(EdgeId)> removal_handler = 0,
    size_t additional_length_bound = 0) {

	if(!br_config.enabled)
		return false;

    INFO("Removing bulges");
    size_t max_length = LengthThresholdFinder::MaxBulgeLength(
        g.k(), br_config.max_bulge_length_coefficient,
        br_config.max_additive_length_coefficient);

    DEBUG("Max bulge length " << max_length);

    if (additional_length_bound != 0 && additional_length_bound < max_length) {
        DEBUG("Setting additional bound " << additional_length_bound);
        max_length = additional_length_bound;
    }

    BulgeRemover<Graph> br(g, max_length, br_config.max_coverage,
                           br_config.max_relative_coverage, br_config.max_delta,
                           br_config.max_relative_delta,
                           opt_handler, removal_handler);

    return br.Process();
}

template<class Graph>
void RemoveLowCoverageEdges(
    Graph &g,
    const debruijn_config::simplification::erroneous_connections_remover& ec_config,
    const SimplifInfoContainer& info_container,
    boost::function<void(EdgeId)> removal_handler = 0) {

    INFO("Removing low covered connections");
    //double max_coverage = cfg::get().simp.ec.max_coverage;
    ConditionParser<Graph> parser(g, ec_config.condition, info_container);

    auto condition = parser();

    omnigraph::IterativeLowCoverageEdgeRemover<Graph> erroneous_edge_remover(
        g, parser.max_coverage_bound(), condition, removal_handler);
    erroneous_edge_remover.Process();

    DEBUG("Low coverage edges removed");
}

template<class Graph>
void RemoveSelfConjugateEdges(
    Graph &g, size_t max_length, double max_coverage,
                boost::function<void(EdgeId)> removal_handler = 0) {
    INFO("Removing short low covered self-conjugate connections");
    LowCoveredSelfConjEdgeRemovingAlgorithm<Graph> algo(g, max_length, max_coverage, removal_handler);
    algo.Process();
    DEBUG("Short low covered self-conjugate connections removed");
}

template<class Graph>
bool RemoveRelativelyLowCoverageComponents(
        Graph &g,
        const FlankingCoverage<Graph>& flanking_cov,
        const debruijn_config::simplification::relative_coverage_comp_remover& rcc_config,
        const SimplifInfoContainer& info,
        typename ComponentRemover<Graph>::HandlerF removal_handler = 0) {
    if (rcc_config.enabled) {
        INFO("Removing relatively low covered connections");
        size_t connecting_path_length_bound = LengthThresholdFinder::MaxErroneousConnectionLength(
            g.k(), rcc_config.max_ec_length_coefficient);

        omnigraph::simplification::relative_coverage::RelativeCoverageComponentRemover<
                Graph> rel_rem(
                g,
                boost::bind(&FlankingCoverage<Graph>::LocalCoverage,
                            boost::cref(flanking_cov), _1, _2),
                            rcc_config.coverage_gap, size_t(double(info.read_length()) * rcc_config.length_coeff),
                            size_t(double(info.read_length()) * rcc_config.tip_allowing_length_coeff),
                            connecting_path_length_bound,
                            info.detected_coverage_bound() * rcc_config.max_coverage_coeff,
                            removal_handler, rcc_config.vertex_count_limit);
        return rel_rem.Process();
    } else {
        INFO("Removal of relatively low covered connections disabled");
        return false;
    }
}

template<class Graph>
bool TopologyRemoveErroneousEdges(
    Graph &g,
    const debruijn_config::simplification::topology_based_ec_remover& tec_config,
    boost::function<void(typename Graph::EdgeId)> removal_handler) {
    INFO("Removing connections based on topology");
    size_t max_length = LengthThresholdFinder::MaxErroneousConnectionLength(
        g.k(), tec_config.max_ec_length_coefficient);
    return omnigraph::TopologyChimericEdgeRemover<Graph>(
        g, max_length, tec_config.uniqueness_length,
        tec_config.plausibility_length, removal_handler).Process();
}

template<class Graph>
bool TopologyClipTips(
    Graph &g,
    const debruijn_config::simplification::topology_tip_clipper& ttc_config,
    size_t read_length,
    boost::function<void(typename Graph::EdgeId)> removal_handler) {
    INFO("Clipping tips based on topology");

    size_t max_length = LengthThresholdFinder::MaxTipLength(
        read_length, g.k(), ttc_config.length_coeff);

    return TopologyTipClipper<Graph>(g, max_length,
                                     ttc_config.uniqueness_length,
                                     ttc_config.plausibility_length,
                                     removal_handler).Process();
}

template<class Graph>
bool MultiplicityCountingRemoveErroneousEdges(
    Graph &g,
    const debruijn_config::simplification::topology_based_ec_remover& tec_config,
    boost::function<void(typename Graph::EdgeId)> removal_handler) {
    INFO("Removing connections based on topological multiplicity counting");
    size_t max_length = LengthThresholdFinder::MaxErroneousConnectionLength(
        g.k(), tec_config.max_ec_length_coefficient);
    return omnigraph::SimpleMultiplicityCountingChimericEdgeRemover<Graph>(
        g, max_length, tec_config.uniqueness_length,
        tec_config.plausibility_length, removal_handler).Process();
}

template<class Graph>
bool RemoveThorns(
    Graph &g,
    const debruijn_config::simplification::interstrand_ec_remover& isec_config,
    boost::function<void(typename Graph::EdgeId)> removal_handler) {
    INFO("Removing interstrand connections");
    size_t max_unr_length = LengthThresholdFinder::MaxErroneousConnectionLength(
        g.k(), isec_config.max_ec_length_coefficient);
    return ThornRemover<Graph>(g, max_unr_length, isec_config.uniqueness_length,
                               isec_config.span_distance, removal_handler)
            .Process();
}

template<class Graph>
bool TopologyReliabilityRemoveErroneousEdges(
    Graph &g,
    const debruijn_config::simplification::tr_based_ec_remover& trec_config,
    boost::function<void(typename Graph::EdgeId)> removal_handler) {
    INFO("Removing connections based on topology and reliable coverage");
    size_t max_unr_length = LengthThresholdFinder::MaxErroneousConnectionLength(
        g.k(), trec_config.max_ec_length_coefficient);
    return TopologyAndReliablityBasedChimericEdgeRemover<Graph>(
        g, max_unr_length, trec_config.uniqueness_length,
        trec_config.unreliable_coverage, removal_handler).Process();
}

template<class Graph>
bool MaxFlowRemoveErroneousEdges(
    Graph &g,
    const debruijn_config::simplification::max_flow_ec_remover& mfec_config,
    boost::function<void(typename Graph::EdgeId)> removal_handler = 0) {
    if (!mfec_config.enabled)
        return false;
    INFO("Removing connections based on max flow strategy");
    size_t max_length = LengthThresholdFinder::MaxErroneousConnectionLength(
        g.k(), (size_t) mfec_config.max_ec_length_coefficient);
    omnigraph::MaxFlowECRemover<Graph> erroneous_edge_remover(
        g, max_length, mfec_config.uniqueness_length,
        mfec_config.plausibility_length, removal_handler);
    return erroneous_edge_remover.Process();
}

template<class Graph>
bool RemoveComplexBulges(
    Graph& g,
    debruijn_config::simplification::complex_bulge_remover cbr_config,
    size_t iteration = 0) {
    if (!cbr_config.enabled)
        return false;
    INFO("Removing complex bulges");
    size_t max_length = (size_t) ((double) g.k() * cbr_config.max_relative_length);
    size_t max_diff = cbr_config.max_length_difference;
    string output_dir = "";
    if (cbr_config.pics_enabled) {
        output_dir = cbr_config.folder;
        make_dir(output_dir);
        output_dir += ToString(iteration) + "/";
    }
    omnigraph::complex_br::ComplexBulgeRemover<Graph> complex_bulge_remover(
        g, max_length, max_diff, output_dir);
    return complex_bulge_remover.Run();
}

template<class Graph>
bool RemoveHiddenEC(Graph& g,
                    const FlankingCoverage<Graph>& flanking_cov,
                    double determined_coverage_threshold,
                    debruijn_config::simplification::hidden_ec_remover her_config,
                    boost::function<void(EdgeId)> removal_handler) {
    if (her_config.enabled) {
        INFO("Removing hidden erroneous connections");
        return HiddenECRemover<Graph>(g, her_config.uniqueness_length, flanking_cov,
                               her_config.unreliability_threshold, determined_coverage_threshold,
                               cfg::get().simp.her.relative_threshold, removal_handler).Process();
    }
    return false;
}

template<class Graph>
bool AllTopology(Graph &g,
                 boost::function<void(typename Graph::EdgeId)> removal_handler,
                 size_t /*iteration*/) {
    bool res = TopologyRemoveErroneousEdges(g, cfg::get().simp.tec,
                                            removal_handler);
    res |= TopologyReliabilityRemoveErroneousEdges(g, cfg::get().simp.trec,
                                                   removal_handler);
    res |= RemoveThorns(g, cfg::get().simp.isec, removal_handler);
    res |= MultiplicityCountingRemoveErroneousEdges(g, cfg::get().simp.tec,
                                                    removal_handler);
    return res;
}

template<class Graph>
bool RemoveIsolatedEdges(Graph &g, size_t max_length, double max_coverage, size_t max_length_any_cov,
                 boost::function<void(typename Graph::EdgeId)> removal_handler = 0) {
    typedef typename Graph::EdgeId EdgeId;

    return EdgeRemovingAlgorithm<Graph>(g,
        func::And<EdgeId>(
            make_shared<IsolatedEdgeCondition<Graph>>(g),
            func::Or<EdgeId>(
                make_shared<LengthUpperBound<Graph>>(g, max_length_any_cov),
                func::And<EdgeId>(
                    make_shared<LengthUpperBound<Graph>>(g, max_length),
                    make_shared<CoverageUpperBound<Graph>>(g, max_coverage)
                )
    )), removal_handler).Process();
}

template<class Graph>
bool RemoveIsolatedEdges(Graph &g, debruijn_config::simplification::isolated_edges_remover ier,
                 size_t read_length,
                 boost::function<void(typename Graph::EdgeId)> removal_handler = 0) {
    typedef typename Graph::EdgeId EdgeId;
    size_t max_length = std::max(read_length, cfg::get().simp.ier.max_length_any_cov);
    //todo add info that some other edges might be removed =)
    INFO("Removing isolated edges");
    INFO("All edges shorter than " << max_length << " will be removed");
    INFO("Also edges shorter than " << ier.max_length << " and coverage smaller than " << ier.max_coverage << " will be removed");
    return RemoveIsolatedEdges(g, ier.max_length, ier.max_coverage, max_length, removal_handler);
}

//todo move to some of the utils files
template<class Graph>
class CountingCallback {
    typedef typename Graph::EdgeId EdgeId;
    std::atomic<size_t> cnt_;

public:
    CountingCallback() : 
            cnt_(0) {
    }
    
    void HandleDelete(EdgeId /*e*/) {
        cnt_++;
    }

    void Report() {
        TRACE(cnt_ << " edges were removed.")
        cnt_ = 0;
    }

private:
    DECL_LOGGER("CountingCallback");
};

template<class gp_t>
bool FinalRemoveErroneousEdges(
    gp_t &gp,
    boost::function<void(typename Graph::EdgeId)> removal_handler,
    const SimplifInfoContainer& info,
    size_t iteration) {

//    gp.ClearQuality();
//    gp.FillQuality();
//    auto colorer = debruijn_graph::DefaultGPColorer(gp);
//    omnigraph::DefaultLabeler<typename gp_t::graph_t> labeler(gp.g, gp.edge_pos);
//    QualityEdgeLocalityPrintingRH<Graph> qual_removal_handler(gp.g, gp.edge_qual, labeler, colorer,
//                                   cfg::get().output_dir + "pictures/colored_edges_deleted/");
//
//    //positive quality edges removed (folder colored_edges_deleted)
//    boost::function<void(EdgeId)> qual_removal_handler_f = boost::bind(
//            //            &QualityLoggingRemovalHandler<Graph>::HandleDelete,
//            &QualityEdgeLocalityPrintingRH<Graph>::HandleDelete,
//            boost::ref(qual_removal_handler), _1);
//
//    boost::function<void(set<EdgeId>)> set_removal_handler_f = boost::bind(
//                &omnigraph::simplification::SingleEdgeAdapter<set<EdgeId>>, _1, qual_removal_handler_f);
//

    boost::function<void(set<EdgeId>)> set_removal_handler_f(0);
    if (removal_handler) {
        set_removal_handler_f = boost::bind(
                &omnigraph::simplification::SingleEdgeAdapter<set<EdgeId>>, _1, removal_handler);
    }

    bool changed = RemoveRelativelyLowCoverageComponents(gp.g, gp.flanking_cov,
                                          cfg::get().simp.rcc, info, set_removal_handler_f);

    if (cfg::get().simp.topology_simplif_enabled && cfg::get().main_iteration) {
        changed |= AllTopology(gp.g, removal_handler, iteration);
        changed |= MaxFlowRemoveErroneousEdges(gp.g, cfg::get().simp.mfec,
                                               removal_handler);
    }
    return changed;
}

template<class Graph>
void Compress(Graph& g) {
    Compressor<Graph> compressor(g);
    compressor.CompressAllVertices();
}

template<class Graph>
void ParallelCompress(Graph& g, const SimplifInfoContainer& info) {
    debruijn::simplification::ParallelCompressor<Graph> compressor(g);
    debruijn::simplification::TwoStepAlgorithmRunner<Graph, typename Graph::VertexId> runner(g, false);
    debruijn::simplification::RunVertexAlgorithm(g, runner, compressor, info.chunk_cnt());
    //have to call "final" compression to get rid of loops
    Compress(g);
}

template<class Graph>
bool ParallelClipTips(Graph& g,
              const string& tip_condition,
              const SimplifInfoContainer& info,
              boost::function<void(typename Graph::EdgeId)> removal_handler = 0) {
    INFO("Parallel tip clipping");

    string condition_str = tip_condition;

    ConditionParser<Graph> parser(g, condition_str, info);

    parser();

    debruijn::simplification::ParallelTipClippingFunctor<Graph> tip_clipper(g, 
        parser.max_length_bound(), parser.max_coverage_bound(), removal_handler);
    
    debruijn::simplification::AlgorithmRunner<Graph, typename Graph::VertexId> runner(g);

    debruijn::simplification::RunVertexAlgorithm(g, runner, tip_clipper, info.chunk_cnt());

    ParallelCompress(g, info);

    Cleaner<Graph> cleaner(g);
    cleaner.Clean();

    return true;
}

//template<class Graph>
//bool ParallelRemoveBulges(Graph& g,
//              const debruijn_config::simplification::bulge_remover& br_config,
//              size_t /*read_length*/,
//              boost::function<void(typename Graph::EdgeId)> removal_handler = 0) {
//    INFO("Parallel bulge remover");
//
//    size_t max_length = LengthThresholdFinder::MaxBulgeLength(
//        g.k(), br_config.max_bulge_length_coefficient,
//        br_config.max_additive_length_coefficient);
//
//    DEBUG("Max bulge length " << max_length);
//
//    debruijn::simplification::ParallelSimpleBRFunctor<Graph> bulge_remover(g,
//                            max_length,
//                            br_config.max_coverage,
//                            br_config.max_relative_coverage,
//                            br_config.max_delta,
//                            br_config.max_relative_delta,
//                            removal_handler);
//    for (VertexId v : g) {
//        bulge_remover(v);
//    }
//
//    Compress(g);
//    return true;
//}

template<class Graph>
bool ParallelEC(Graph& g,
              const string& ec_condition,
              const SimplifInfoContainer& info,
              boost::function<void(typename Graph::EdgeId)> removal_handler = 0) {
    INFO("Parallel ec remover");

    ConditionParser<Graph> parser(g, ec_condition, info);

    auto condition = parser();

    size_t max_length = parser.max_length_bound();
    double max_coverage = parser.max_coverage_bound();

    debruijn::simplification::ParallelLowCoverageFunctor<Graph> ec_remover(g,
                            max_length,
                            max_coverage,
                            removal_handler);

    debruijn::simplification::TwoStepAlgorithmRunner<Graph, typename Graph::EdgeId> runner(g, true);

    debruijn::simplification::RunEdgeAlgorithm(g, runner, ec_remover, info.chunk_cnt());

    ParallelCompress(g, info);
    
    //Not executing cleaner as small optimization =) Not so many vertices to clean here.

    return true;
}

inline
void NonParallelPreSimplification(conj_graph_pack& gp,
                       const debruijn_config::simplification::presimplification& presimp,
                       const SimplifInfoContainer& info,
                       boost::function<void(EdgeId)> removal_handler) {
    INFO("Non parallel mode");
    CountingCallback<Graph> cnt_callback;

    boost::function<void(EdgeId)> cnt_handler = boost::bind(&CountingCallback<Graph>::HandleDelete, boost::ref(cnt_callback), _1); 
    removal_handler = boost::bind(func::Composition<EdgeId>, _1, removal_handler,
                       cnt_handler);

    debruijn_config::simplification::tip_clipper tc_config;
    tc_config.condition = presimp.tip_condition;

    ClipTips(gp, tc_config, info, false, removal_handler);
    
    cnt_callback.Report();

    debruijn_config::simplification::erroneous_connections_remover ec_config;
    ec_config.condition = presimp.ec_condition;

    RemoveLowCoverageEdges(gp.g, ec_config, info, removal_handler);

    cnt_callback.Report();
}

inline
void ParallelPreSimplification(conj_graph_pack& gp,
                       const debruijn_config::simplification::presimplification& presimp,
                       const SimplifInfoContainer& info,
                       boost::function<void(EdgeId)> removal_handler) {
    INFO("Parallel mode");
    CountingCallback<Graph> cnt_callback;

    boost::function<void(EdgeId)> cnt_handler = boost::bind(&CountingCallback<Graph>::HandleDelete, boost::ref(cnt_callback), _1); 
    removal_handler = boost::bind(func::Composition<EdgeId>, _1, removal_handler,
                       cnt_handler);

    ParallelClipTips(gp.g, presimp.tip_condition, info,
                     removal_handler);
    
    cnt_callback.Report();
    //    INFO("Early tip clipping");
    //
    //    ClipTipsWithProjection(gp, cfg::get().simp.tc,
    //                           cfg::get().graph_read_corr.enable, cfg::get().ds.RL(),
    //                           determined_coverage_threshold, removal_handler);
    //


//    ParallelRemoveBulges(gp.g, cfg::get().simp.br, cfg::get().ds.RL(),
//                         removal_handler);
//
//    cnt_callback.Report();

    ParallelEC(gp.g, presimp.ec_condition, info,
               removal_handler);

    cnt_callback.Report();

    //todo maybe enable with small
//    INFO("Isolated edge remover");
//    size_t max_length = std::max(cfg::get().ds.RL(), cfg::get().simp.ier.max_length_any_cov);
//    INFO("All edges of length smaller than " << max_length << " will be removed");
//    IsolatedEdgeRemover<Graph>(gp.g, cfg::get().simp.ier.max_length,
//                               cfg::get().simp.ier.max_coverage, max_length)
//            .RemoveIsolatedEdges();
//
//    INFO("Early bulge removal");
//    RemoveBulges(gp.g, cfg::get().simp.br, 0, removal_handler, gp.g.k() + 1);
}

inline
bool EnableParallel(const conj_graph_pack& gp,
                       const debruijn_config::simplification::presimplification& presimp) {
    if (presimp.parallel) {
        INFO("Trying to enable parallel presimplification. Chunk count = " << presimp.chunk_cnt);
        VERIFY(presimp.chunk_cnt > 0);
        if (presimp.chunk_cnt == 1) {
            return true;
        } else {
            if (gp.g.AllHandlersThreadSafe()) {
                return true;
            } else {
                INFO("Not all handlers are threadsafe, switching to non-parallel presimplif");
                //gp.g.PrintHandlersNames();
            }
        }
    }
    return false;
}

inline
void PreSimplification(conj_graph_pack& gp,
                       const debruijn_config::simplification::presimplification& presimp,
                       const SimplifInfoContainer& info,
                       boost::function<void(EdgeId)> removal_handler) {
    INFO("PROCEDURE == Presimplification");

    if (!presimp.enabled) {
        INFO("Further presimplification is disabled");
        return;
    }
    
    RemoveSelfConjugateEdges(gp.g, gp.k_value + 100, 1., removal_handler);
    //todo make parallel version
    RemoveIsolatedEdges(gp.g, presimp.ier, info.read_length(), removal_handler);

    if (math::eq(info.detected_mean_coverage(), 0.)) {
    	INFO("Mean coverage wasn't reliably estimated, no further presimplification");
    	return;
    } else if (math::ls(info.detected_mean_coverage(), presimp.activation_cov)) {
    	INFO("Estimated mean coverage " << info.detected_mean_coverage()
    			<< " is less than activation coverage " << presimp.activation_cov
    			<< ", no further presimplification");
    	return;
    }
    

    if (EnableParallel(gp, presimp)) {
        //fixme chunks configuration
        SimplifInfoContainer presimp_info(info); 

        ParallelPreSimplification(gp, presimp, presimp_info.set_chunk_cnt(presimp.chunk_cnt), removal_handler);
    } else {
        NonParallelPreSimplification(gp, presimp, info, removal_handler);
    }

}

inline
void PostSimplification(conj_graph_pack& gp,
                        const SimplifInfoContainer& info,
                        boost::function<void(EdgeId)> &removal_handler,
                        stats::detail_info_printer& /*printer*/) {

    INFO("PROCEDURE == Post simplification");
    size_t iteration = 0;
    bool enable_flag = true;
    while (enable_flag) {
        enable_flag = false;

        INFO("Iteration " << iteration);
        if (cfg::get().simp.topology_simplif_enabled) {
            enable_flag |= TopologyClipTips(gp.g, cfg::get().simp.ttc, info.read_length(),
                                            removal_handler);
        }

        enable_flag |= FinalRemoveErroneousEdges(gp, removal_handler,
                                                 info,
                                                 iteration);

        enable_flag |= ClipTips(gp, cfg::get().simp.tc,
                                              info,
                                              cfg::get().graph_read_corr.enable,
                                              removal_handler);

        enable_flag |= RemoveBulges(gp.g, cfg::get().simp.br, 0, removal_handler);

        enable_flag |= RemoveComplexBulges(gp.g, cfg::get().simp.cbr, iteration);

        iteration++;

        //    printer(ipp_before_final_err_con_removal);
        //        printer(ipp_final_tip_clipping, str(format("_%d") % iteration));
        //        printer(ipp_final_err_con_removal, str(format("_%d") % iteration));
        //        printer(ipp_final_bulge_removal, str(format("_%d") % iteration));
    }

    if (cfg::get().simp.topology_simplif_enabled) {
        RemoveHiddenEC(gp.g, gp.flanking_cov, info.detected_coverage_bound(), cfg::get().simp.her, removal_handler);
    }
}

inline
void IdealSimplification(Graph& graph, Compressor<Graph>& compressor,
                         boost::function<double(EdgeId)> quality_handler_f) {
    for (auto iterator = graph.SmartEdgeBegin(); !iterator.IsEnd();
         ++iterator) {
        if (math::eq(quality_handler_f(*iterator), 0.))
            graph.DeleteEdge(*iterator);
    }
    compressor.CompressAllVertices();
}

inline
void SimplificationCycle(conj_graph_pack& gp,
                         const SimplifInfoContainer& info_container,
                         boost::function<void(EdgeId)> removal_handler,
                         stats::detail_info_printer &printer) {
    size_t iteration = info_container.iteration();

    INFO("PROCEDURE == Simplification cycle, iteration " << (iteration + 1));
    
    CountingCallback<Graph> cnt_callback;

    boost::function<void(EdgeId)> cnt_handler = boost::bind(&CountingCallback<Graph>::HandleDelete, boost::ref(cnt_callback), _1); 
    removal_handler = boost::bind(func::Composition<EdgeId>, _1, removal_handler,
                       cnt_handler);

    DEBUG(iteration << " TipClipping");
    ClipTips(gp, cfg::get().simp.tc,
                           info_container,
                           cfg::get().graph_read_corr.enable, removal_handler);
    cnt_callback.Report();
    DEBUG(iteration << " TipClipping stats");
    printer(ipp_tip_clipping, str(format("_%d") % iteration));

    DEBUG(iteration << " BulgeRemoval");
    RemoveBulges(gp.g, cfg::get().simp.br, 0, removal_handler);
    cnt_callback.Report();
    DEBUG(iteration << " BulgeRemoval stats");
    printer(ipp_bulge_removal, str(format("_%d") % iteration));

    DEBUG(iteration << " ErroneousConnectionsRemoval");
    RemoveLowCoverageEdges(gp.g, cfg::get().simp.ec, info_container, removal_handler);
    cnt_callback.Report();
    DEBUG(iteration << " ErroneousConnectionsRemoval stats");
    printer(ipp_err_con_removal, str(format("_%d") % iteration));

}

inline
void SimplifyGraph(conj_graph_pack &gp,
                   boost::function<void(EdgeId)> removal_handler,
                   stats::detail_info_printer& printer, size_t iteration_count) {
    printer(ipp_before_simplification);
    DEBUG("Graph simplification started");

    SimplifInfoContainer info_container;
    info_container
        .set_detected_coverage_bound(gp.ginfo.ec_bound())
        //0 if model didn't converge
        .set_detected_mean_coverage(gp.ginfo.estimated_mean())
        .set_read_length(cfg::get().ds.RL());

    PreSimplification(gp, cfg::get().simp.presimp,
    		info_container, removal_handler);

    info_container.set_iteration_count(iteration_count);
    for (size_t i = 0; i < iteration_count; i++) {
        info_container.set_iteration(i);
        SimplificationCycle(gp, info_container, removal_handler, printer);
    }

    PostSimplification(gp, info_container, removal_handler, printer);
}

}

}
