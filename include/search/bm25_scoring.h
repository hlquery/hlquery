/*
 * hlquery - Search beyond keywords.
 * https://www.hlquery.com
 *
 * Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
 *
 * This file is part of hlquery, released under the BSD License version 3.
 */

#pragma once

#include <cmath>
#include <string>

namespace BM25Scoring
{
/* Log1pRatio computes log(1 + Numerator / Denominator) without overflowing the division. */

inline double Log1pRatio(double Numerator, double Denominator)
{
     if (!std::isfinite(Numerator) || !std::isfinite(Denominator) ||
         Numerator < 0.0 || Denominator <= 0.0)
     {
          return 0.0;
     }

     if (Numerator <= Denominator)
     {
          return std::log1p(Numerator / Denominator);
     }

     return (std::log(Numerator) - std::log(Denominator)) +
            std::log1p(Denominator / Numerator);
}

/* CalculateIdf computes the configured BM25 IDF without forming unstable large ratios. */

inline double CalculateIdf(double DocFreq, double CollectionSize,
                           const std::string &IdfMode, double IdfSmooth,
                           bool ClampNegative, double IdfFloorFactor)
{
     if (!std::isfinite(DocFreq) || !std::isfinite(CollectionSize) ||
         !std::isfinite(IdfSmooth) || !std::isfinite(IdfFloorFactor) ||
         DocFreq <= 0.0 || CollectionSize <= 0.0 || DocFreq > CollectionSize)
     {
          return 0.0;
     }

     double Idf = 0.0;

     if (IdfMode == "smooth")
     {
          const double Numerator = CollectionSize - DocFreq + 0.5;
          const double Denominator = DocFreq + 0.5 + IdfSmooth;
          Idf = Log1pRatio(Numerator, Denominator);
     }
     else
     {
          const double Numerator = CollectionSize - DocFreq + 0.5;
          const double Denominator = DocFreq + 0.5;

          if (Numerator <= 0.0 || !std::isfinite(Denominator) || Denominator <= 0.0)
          {
               return 0.0;
          }

          Idf = std::log(Numerator) - std::log(Denominator);

          if (ClampNegative && Idf < 0.0)
          {
               Idf = IdfFloorFactor * Log1pRatio(CollectionSize, DocFreq);
          }
     }

     return std::isfinite(Idf) ? Idf : 0.0;
}
}
