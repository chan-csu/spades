#ifndef ADVANCED_DISTANCE_ESTIMATION_HPP_
#define ADVANCED_DISTANCE_ESTIMATION_HPP_

#include "paired_info.hpp"
#include "omni_utils.hpp"
#include "data_divider.hpp"
#include "peak_finder.hpp"

namespace omnigraph {

template<class Graph>
class AdvancedDistanceEstimator: AbstractDistanceEstimator<Graph> {
private:
	typedef typename Graph::EdgeId EdgeId;
	Graph &graph_;
	PairedInfoIndex<Graph> &histogram_;
	size_t insert_size_;
	size_t read_length_;
	size_t gap_;
	size_t delta_;
	size_t linkage_distance_;
	size_t max_distance_;

	const vector<size_t> GetGraphDistances(EdgeId first, EdgeId second) {
		DifferentDistancesCallback<Graph> callback(graph_);
		PathProcessor<Graph> path_processor(graph_, omnigraph::PairInfoPathLengthLowerBound(graph_.k(), graph_.length(first), graph_.length(second), gap_, delta_),
				omnigraph::PairInfoPathLengthUpperBound(graph_.k(), insert_size_, delta_)
				//				0. + gap_  + 2 * (graph_.k() + 1) - graph_.k() - delta_ - graph_.length(first) - graph_.length(
				//						second), insert_size_ - graph_.k() - 2 + delta_
						, graph_.EdgeEnd(first), graph_.EdgeStart(second), callback);
		path_processor.Process();
		auto result = callback.distances();
		for (size_t i = 0; i < result.size(); i++) {
			result[i] += graph_.length(first);
		}
		if (first == second) {
			result.push_back(0);
		}
		sort(result.begin(), result.end());
		return result;
	}

	const static int CUTOFF = 3;
	const static int MINIMALPEAKPOINTS = 2; //the minimal number of points in cluster to be considered consistent

	vector<pair<size_t, double> > EstimateEdgePairDistances(vector<PairInfo<EdgeId> > data, vector<size_t> forward) {
		vector<pair<size_t, double> > result;
        if (data.size() <= 1) return result;
		std::vector<int> clusters = divideData(data);
		std::vector<int> peaks;
		size_t cur = 0;
		for (size_t i = 0; i < clusters.size() - 1; i++) {
			if (clusters[i + 1] - clusters[i] > MINIMALPEAKPOINTS) {
				size_t begin = clusters[i];
				size_t end = clusters[i + 1];
				while (cur<forward.size() && forward[cur] < data[begin].d)
					cur++;
				PeakFinder peakfinder(data, begin, end);
//				std::cout << "Processing window : " << x[array[i]] << " " << x[array[i + 1] - 1] << std::endl;
				peakfinder.FFTSmoothing(CUTOFF);
				while (cur<forward.size() && forward[cur] <= data[end - 1].d) {
					if (peakfinder.isPeak(forward[cur])) result.push_back(make_pair(forward[cur], 10000));
					cur++;
				}
			}
		}
		return result;
	}

	vector<PairInfo<EdgeId> > ClusterResult(EdgeId edge1, EdgeId edge2,
			vector<pair<size_t, double> > estimated) {
		vector < PairInfo < EdgeId >> result;
		for (size_t i = 0; i < estimated.size(); i++) {
			size_t left = i;
			double weight = estimated[i].second;
			while (i + 1 < estimated.size() && estimated[i + 1].first - estimated[i].first
					<= linkage_distance_) {
				i++;
				weight += estimated[i].second;
			}
			double center = (estimated[left].first + estimated[i].first) * 0.5;
			double var = (estimated[i].first - estimated[left].first) * 0.5;
			PairInfo<EdgeId> new_info(edge1, edge2, center, weight, var);
			result.push_back(new_info);
		}
		return result;
	}

	void AddToResult(PairedInfoIndex<Graph> &result, vector<PairInfo<EdgeId> > clustered) {
		for(auto it = clustered.begin(); it != clustered.end(); ++it) {
			result.AddPairInfo(*it);
		}
	}

public:
	AdvancedDistanceEstimator(Graph &graph, PairedInfoIndex<Graph> &histogram, size_t insert_size, size_t read_length, size_t delta, size_t linkage_distance,
			size_t max_distance) :
			graph_(graph), histogram_(histogram), insert_size_(insert_size), read_length_(read_length), gap_(insert_size - 2 * read_length_), delta_(delta), linkage_distance_(
					linkage_distance), max_distance_(max_distance) {
	        INFO("Advanced Estimator started");
    }

	virtual ~AdvancedDistanceEstimator() {
	}

	virtual void Estimate(PairedInfoIndex<Graph> &result) {
		for (auto iterator = histogram_.begin(); iterator != histogram_.end(); ++iterator) {
			vector<PairInfo<EdgeId> > data = *iterator;
			EdgeId first = data[0].first;
			EdgeId second = data[0].second;
			vector<size_t> forward = GetGraphDistances(first, second);
			vector<pair<size_t, double> > estimated = EstimateEdgePairDistances(data, forward);
			vector<PairInfo<EdgeId> > clustered = ClusterResult(first, second, estimated);
			AddToResult(result, clustered);
		}
	}
};

}

#endif /* ADVANCED_DISTANCE_ESTIMATION_HPP_ */
