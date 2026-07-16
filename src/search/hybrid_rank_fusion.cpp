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

#include "search/hybrid_rank_fusion.h"

/*
 * HybridSearchEngine::CombineScores - Blends lexical and vector relevance into a single score.
 */

float HybridSearchEngine::CombineScores(float LexicalScore, float VectorScore, float Alpha)
{
     /*
      * Use a straight weighted blend so callers can tune the tradeoff without
      * changing downstream ranking code. Higher alpha gives more influence to
      * the vector score, while lower alpha keeps lexical relevance dominant.
      */

     return (Alpha * VectorScore) + ((1.0f - Alpha) * LexicalScore);
}

/*
 * HybridSearchEngine::ReciprocalRankFusion - Merges ranked lists using reciprocal rank fusion.
 *
 * Based on Cormack, Clarke, and Buettcher, "Reciprocal Rank Fusion
 * Outperforms Condorcet and Individual Rank Learning Methods", SIGIR 2009.
 */

float HybridSearchEngine::ReciprocalRankFusion(int LexicalRank, int VectorRank, int K)
{
     /*
      * Reciprocal rank fusion rewards documents that appear near the top of
      * either list and gives an extra boost when both systems agree.
      */

     float score = 0.0f;

     if (LexicalRank > 0)
     {
          score += 1.0f / (static_cast<float>(K) + static_cast<float>(LexicalRank));
     }

     if (VectorRank > 0)
     {
          score += 1.0f / (static_cast<float>(K) + static_cast<float>(VectorRank));
     }

     return score;
}
