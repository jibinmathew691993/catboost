#pragma once

#include "learn_context.h"

#include <catboost/libs/options/catboost_options.h>
#include <catboost/libs/data/dataset.h>
#include <catboost/libs/data_types/query.h>

#include <library/fast_exp/fast_exp.h>
#include <library/fast_log/fast_log.h>
#include <library/threading/local_executor/local_executor.h>

#include <util/generic/array_ref.h>
#include <util/generic/vector.h>
#include <util/generic/xrange.h>
#include <util/generic/ymath.h>
#include <util/system/yassert.h>


template <bool StoreExpApprox>
static inline double UpdateApprox(double approx, double approxDelta) {
    return StoreExpApprox ? approx * approxDelta : approx + approxDelta;
}

template <bool StoreExpApprox>
static inline double GetNeutralApprox() {
    return StoreExpApprox ? 1.0 : 0.0;
}

template <bool StoreExpApprox>
static inline double ApplyLearningRate(double approxDelta, double learningRate) {
    return StoreExpApprox ? fast_exp(FastLogf(approxDelta) * learningRate) : approxDelta * learningRate;
}

static inline double GetNeutralApprox(bool storeExpApproxes) {
    if (storeExpApproxes) {
        return GetNeutralApprox</*StoreExpApprox*/ true>();
    } else {
        return GetNeutralApprox</*StoreExpApprox*/ false>();
    }
}

static inline void ExpApproxIf(bool storeExpApproxes, TVector<double>* approx) {
    if (storeExpApproxes) {
        FastExpInplace(approx->data(), approx->ysize());
    }
}

static inline void ExpApproxIf(bool storeExpApproxes, TVector<TVector<double>>* approxMulti) {
    for (auto& approx : *approxMulti) {
        ExpApproxIf(storeExpApproxes, &approx);
    }
}


inline bool IsStoreExpApprox(ELossFunction lossFunction) {
    return EqualToOneOf(
        lossFunction,
        ELossFunction::Logloss,
        ELossFunction::LogLinQuantile,
        ELossFunction::Poisson,
        ELossFunction::CrossEntropy,
        ELossFunction::PairLogit,
        ELossFunction::PairLogitPairwise,
        ELossFunction::YetiRank,
        ELossFunction::YetiRankPairwise
    );
}

inline void CalcPairwiseWeights(const TVector<TQueryInfo>& queriesInfo, int queriesCount, TVector<float>* pairwiseWeights) {
    Fill(pairwiseWeights->begin(), pairwiseWeights->end(), 0);
    for (int queryIndex = 0; queryIndex < queriesCount; ++queryIndex) {
        const auto& queryInfo = queriesInfo[queryIndex];
        for (int docId = 0; docId < queryInfo.Competitors.ysize(); ++docId) {
            for (const auto& competitor : queryInfo.Competitors[docId]) {
                (*pairwiseWeights)[queryInfo.Begin + docId] += competitor.Weight;
                (*pairwiseWeights)[queryInfo.Begin + competitor.Id] += competitor.Weight;
            }
        }
    }
}

template <typename TUpdateFunc>
inline void UpdateApprox(
    const TUpdateFunc& updateFunc,
    const TVector<TVector<double>>& delta,
    TVector<TVector<double>>* approx,
    NPar::TLocalExecutor* localExecutor
) {
    Y_ASSERT(delta.size() == approx->size());
    for (size_t dimensionIdx : xrange(delta.size())) {
        TConstArrayRef<double> deltaDim(delta[dimensionIdx]);
        TArrayRef<double> approxDim((*approx)[dimensionIdx]); // deltaDim.size() < approxDim.size(), if delta is leaf values
        NPar::ParallelFor(*localExecutor, 0, approxDim.size(), [=, &updateFunc](int idx) {
            updateFunc(deltaDim, approxDim, idx);
        });
    }
}

void UpdateAvrgApprox(
    bool storeExpApprox,
    size_t learnSampleCount,
    const TVector<TIndexType>& indices,
    const TVector<TVector<double>>& treeDelta,
    const TDatasetPtrs& testDataPtrs,
    TLearnProgress* learnProgress,
    NPar::TLocalExecutor* localExecutor
);

void NormalizeLeafValues(
    bool isPairwise,
    double learningRate,
    const TVector<double>& leafWeightsSum,
    TVector<TVector<double>>* treeValues
);

inline TVector<double> SumLeafWeights(size_t leafCount,
    const TVector<TIndexType>& leafIndices,
    const TVector<ui32>& learnPermutation,
    const TVector<float>& learnWeights
) {
    TVector<double> weightSum(leafCount);
    for (size_t docIdx = 0; docIdx < learnWeights.size(); ++docIdx) {
        weightSum[leafIndices[learnPermutation[docIdx]]] += learnWeights[docIdx];
    }
    return weightSum;
}

template <typename TElementType>
inline void AddElementwise(const TVector<TElementType>& value, TVector<TElementType>* accumulator) {
    Y_ASSERT(value.size() == accumulator->size());
    for (int idx : xrange(value.size())) {
        AddElementwise(value[idx], &(*accumulator)[idx]);
    }
}

template <>
inline void AddElementwise<double>(const TVector<double>& value, TVector<double>* accumulator) {
    Y_ASSERT(value.size() == accumulator->size());
    for (int idx : xrange(value.size())) {
        (*accumulator)[idx] += value[idx];
    }
}
