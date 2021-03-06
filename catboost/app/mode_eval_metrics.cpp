#include "modes.h"

#include <catboost/libs/algo/plot.h>
#include <catboost/libs/app_helpers/proceed_pool_in_blocks.h>
#include <catboost/libs/helpers/exception.h>
#include <catboost/libs/labels/label_converter.h>
#include <catboost/libs/labels/label_helper_builder.h>
#include <catboost/libs/logging/logging.h>
#include <catboost/libs/metrics/metric.h>
#include <catboost/libs/model/model.h>
#include <catboost/libs/options/analytical_mode_params.h>

#include <library/getopt/small/last_getopt_opts.h>

#include <util/folder/tempdir.h>
#include <util/string/iterator.h>
#include <util/system/compiler.h>


struct TModeEvalMetricsParams {
    ui32 Step = 1;
    ui32 FirstIteration = 0;
    ui32 EndIteration = 0;
    int ReadBlockSize;
    TString MetricsDescription;
    TString ResultDirectory;
    TString TmpDir;

    void BindParserOpts(NLastGetopt::TOpts& parser) {
        parser.AddLongOption("ntree-start", "Start iteration.")
                .RequiredArgument("INT")
                .StoreResult(&FirstIteration);
        parser.AddLongOption("ntree-end", "End iteration.")
                .RequiredArgument("INT")
                .StoreResult(&EndIteration);
        parser.AddLongOption("eval-period", "Eval metrics every eval-period trees.")
                .RequiredArgument("INT")
                .StoreResult(&Step);
        parser.AddLongOption("metrics", "coma-separated eval metrics")
                .RequiredArgument("String")
                .StoreResult(&MetricsDescription);
        parser.AddLongOption("result-dir", "directory with results")
                .RequiredArgument("String")
                .StoreResult(&ResultDirectory);
        parser.AddLongOption("block-size", "Compute block size")
                .RequiredArgument("INT")
                .DefaultValue("150000")
                .StoreResult(&ReadBlockSize);
        parser.AddLongOption("tmp-dir", "Dir to store approx for non-additive metrics. Use \"-\" to generate directory.")
                .RequiredArgument("String")
                .DefaultValue("-")
                .StoreResult(&TmpDir);
    }
};

static void PreprocessTarget(const TLabelConverter& labelConverter, TVector<float>* targets) {
    if (labelConverter.IsInitialized()) {
        PrepareTargetCompressed(labelConverter, targets);
    }
}

static void ReadDatasetParts(
    const NCB::TAnalyticalModeCommonParams& params,
    int blockSize,
    const TLabelConverter& labelConverter,
    NPar::TLocalExecutor* executor,
    TVector<TPool>* datasetParts) {
    ReadAndProceedPoolInBlocks(params, blockSize, [&](TPool& poolPart) {
        PreprocessTarget(labelConverter, &poolPart.Docs.Target);
        datasetParts->emplace_back();
        datasetParts->back().Swap(poolPart);
    },
    executor);
}

static TVector<THolder<IMetric>> CreateMetrics(
    const TModeEvalMetricsParams& plotParams,
    int approxDim) {

    TVector<TString> metricsDescription;
    for (const auto& metricDescription : StringSplitter(plotParams.MetricsDescription).Split(',').SkipEmpty()) {
        metricsDescription.emplace_back(metricDescription.Token());
    }
    CB_ENSURE(!metricsDescription.empty(), "No metric in metrics description " << plotParams.MetricsDescription);

    auto metrics = CreateMetricsFromDescription(metricsDescription, approxDim);
    return metrics;
}

int mode_eval_metrics(int argc, const char* argv[]) {
    NCB::TAnalyticalModeCommonParams params;
    TModeEvalMetricsParams plotParams;
    bool verbose = false;

    auto parser = NLastGetopt::TOpts();
    parser.AddHelpOption();
    params.BindParserOpts(parser);
    plotParams.BindParserOpts(parser);
    parser.AddLongOption("verbose")
        .SetFlag(&verbose)
        .NoArgument();

    bool saveStats = false;
    parser.AddLongOption("save-stats")
        .SetFlag(&saveStats)
        .NoArgument();

    bool calcOnParts = false;
    parser.AddLongOption("calc-on-parts")
        .SetFlag(&calcOnParts)
        .NoArgument();

    parser.SetFreeArgsNum(0);
    {
        NLastGetopt::TOptsParseResult parseResult(&parser, argc, argv);
        Y_UNUSED(parseResult);
    }
    TSetLoggingVerboseOrSilent inThisScope(verbose);

    TFullModel model = ReadModel(params.ModelFileName, params.ModelFormat);
    CB_ENSURE(model.GetUsedCatFeaturesCount() == 0 || params.DsvPoolFormatParams.CdFilePath.Inited(),
              "Model has categorical features. Specify column_description file with correct categorical features.");
    params.ClassNames = GetModelClassNames(model);

    if (plotParams.EndIteration == 0) {
        plotParams.EndIteration = model.ObliviousTrees.TreeSizes.size();
    }
    if (plotParams.TmpDir == "-") {
        plotParams.TmpDir = TTempDir().Name();
    }

    NPar::TLocalExecutor executor;
    executor.RunAdditionalThreads(params.ThreadCount - 1);

    auto metrics = CreateMetrics(plotParams, model.ObliviousTrees.ApproxDimension);

    TMetricsPlotCalcer plotCalcer = CreateMetricCalcer(
        model,
        plotParams.FirstIteration,
        plotParams.EndIteration,
        plotParams.Step,
        /*processedIterationsStep=*/50, // TODO(nikitxskv): Make auto estimation of this parameter based on the free RAM and pool size.
        executor,
        plotParams.TmpDir,
        metrics
    );

    auto labelConverter = BuildLabelsHelper<TLabelConverter>(model);

    TVector<TPool> datasetParts;
    if (plotCalcer.HasAdditiveMetric()) {
        ReadAndProceedPoolInBlocks(params, plotParams.ReadBlockSize, [&](TPool& poolPart) {
            PreprocessTarget(labelConverter, &poolPart.Docs.Target);
            plotCalcer.ProceedDataSetForAdditiveMetrics(poolPart, !poolPart.Docs.QueryId.empty());
            if (plotCalcer.HasNonAdditiveMetric() && !calcOnParts) {
                datasetParts.emplace_back();
                datasetParts.back().Swap(poolPart);
            }
        }, &executor);
        plotCalcer.FinishProceedDataSetForAdditiveMetrics();
    }

    if (plotCalcer.HasNonAdditiveMetric() && calcOnParts) {
        while (!plotCalcer.AreAllIterationsProcessed()) {
            ReadAndProceedPoolInBlocks(params, plotParams.ReadBlockSize, [&](TPool& poolPart) {
                PreprocessTarget(labelConverter, &poolPart.Docs.Target);
                plotCalcer.ProceedDataSetForNonAdditiveMetrics(poolPart);
            }, &executor);
            plotCalcer.FinishProceedDataSetForNonAdditiveMetrics();
        }
    }

    if (plotCalcer.HasNonAdditiveMetric() && !calcOnParts) {
        if (datasetParts.empty()) {
            ReadDatasetParts(params, plotParams.ReadBlockSize, labelConverter, &executor, &datasetParts);
        }
        plotCalcer.ComputeNonAdditiveMetrics(datasetParts);
    }
    plotCalcer.SaveResult(plotParams.ResultDirectory, params.OutputPath.Path, true /*saveMetrics*/, saveStats).ClearTempFiles();
    return 0;
}
