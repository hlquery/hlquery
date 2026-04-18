/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 * You are free to redistribute and/or modify this software
 * under the terms of the BSD License.
 * For more details, please visit: https://docs.hlquery.com
 */

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

/* Runtime settings for the AISearch module. */

struct AISearchConfig
{
     std::string TitleField = "title";
     std::string DescriptionField = "description";
     std::string LabelsField = "labels";
     double TitlePhraseWeight = 8.0;
     double DescriptionPhraseWeight = 6.0;
     double LabelPhraseWeight = 9.0;
     double TitleExactTokenWeight = 2.0;
     double TitleFuzzyTokenWeight = 0.75;
     double DescriptionExactTokenWeight = 1.25;
     double DescriptionFuzzyTokenWeight = 0.5;
     double LabelExactTokenWeight = 2.5;
     double LabelFuzzyTokenWeight = 1.0;
     bool EnableIDFWeighting = true;
     double IDFWeightStrength = 1.0;
     double TitleExactMatchBoost = 1.75;
     double LabelExactMatchBoost = 1.5;
     double DescriptionExactMatchBoost = 1.0;
     double FuzzyIDFScale = 0.35;
     int DefaultLimit = 10;
     int MaxLimit = 50;
     int BatchSize = 200;
     int MaxScanDocsPerCollection = 5000;
     std::unordered_map<std::string, std::vector<std::string>> SynonymMap;
     std::vector<std::string> FocusLabels;
     double FocusLabelBoost = 3.0;
     bool EnableAutomaticIntentLabels = true;
     double AutomaticIntentLabelBoost = 2.5;
     bool EnableBoilerplateTitlePenalty = true;
     double BoilerplateTitlePenalty = 1.5;
     bool EnableSemanticExpansion = true;
     int SemanticExpansionTermLimit = 6;
     bool EnableInferredSynonyms = true;
     int InferredSynonymTermLimit = 8;
     bool EnableZeroResultInferredSynonymFallback = true;
     int ZeroResultInferredSynonymCollectionScoreThreshold = 2;
     bool EnableLLMFirstIntent = true;
     bool EnableLLMTalkReply = true;
     bool EnableLLMRerank = false;
     int LLMRerankTopK = 10;
     double LLMRerankWeight = 8.0;
     bool EnableResultPruning = true;
     std::string ResultPruningMode = "top_relative";
     double ResultPruningRatio = 0.8;
     bool EnableFlatTailPruning = true;
     double FlatTailScoreThreshold = 1.5;
     double TieDensityThreshold = 0.6;
     bool EnableCommonTokenSuppression = true;
     double CommonTokenDocFrequencyRatio = 0.6;
     int CommonTokenMinDocuments = 3;
     bool EnableMinimumQualityGate = true;
     int MinimumQualityTokenMatches = 2;
};
