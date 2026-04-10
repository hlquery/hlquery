/*
 * hlquery - Search beyond keywords.
 * http://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 * You are free to redistribute and/or modify this software
 * under the terms of the BSD License.
 * For more details, please visit: https://docs.hlquery.com
 */

#pragma once

#include <map>
#include <string>
#include <vector>

/*
 * HybridSearchEngine - Combines lexical and vector search results.
 */

/*
 * HybridSearchEngine - Combines lexical and vector search results.
 * Score = alpha * vector_score + (1 - alpha) * lexical_score.
 */

class HybridSearchEngine
{
   public:

     /* CombineScores merges lexical and vector scores. */

     static float CombineScores(float LexicalScore, float VectorScore, float Alpha);

     /* ReciprocalRankFusion combines results using RRF. */

     static float ReciprocalRankFusion(int LexicalRank, int VectorRank, int K = 60);
};
