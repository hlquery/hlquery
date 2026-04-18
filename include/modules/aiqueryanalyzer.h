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

#include <algorithm>
#include <cctype>
#include <rapidfuzz/fuzz.hpp>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hlquery_ai_query
{
struct EntityAnalysis
{
     std::string NormalizedSubject;
     std::string EntityType = "unknown";
     std::vector<std::string> PossibleSynonyms;
     std::vector<std::string> RelatedTerms;
};

struct QueryRewritePlan
{
     std::vector<std::string> RewritePhrases;
     std::vector<std::string> RewriteReasons;
};

struct VagueQueryPlan
{
     bool IsVague = false;
     std::string Reason;
     std::vector<std::string> ClarifyingTerms;
     std::vector<std::string> SuggestedQueries;
    std::vector<std::string> FollowUpQuestions;
};

struct ExclusionPlan
{
     bool HasExclusions = false;
     std::string Reason;
     std::vector<std::string> Terms;
};

inline std::string ToLowerCopy(std::string Value)
{
     std::transform(Value.begin(), Value.end(), Value.begin(),
                    [](unsigned char C)
                    {
                         return static_cast<char>(std::tolower(C));
                    });
     return Value;
}

inline std::string TrimCopy(const std::string &Value)
{
     const size_t Start = Value.find_first_not_of(" \t\r\n");
     if (Start == std::string::npos)
     {
          return "";
     }

     const size_t End = Value.find_last_not_of(" \t\r\n");
     return Value.substr(Start, End - Start + 1);
}

inline std::string TrimTrailingPunctuation(std::string Value)
{
     while (!Value.empty() && (Value.back() == '.' || Value.back() == '?' || Value.back() == '!' || Value.back() == ',' || Value.back() == ';' || Value.back() == ':'))
     {
          Value.pop_back();
     }

     return TrimCopy(Value);
}

inline void AppendUnique(std::vector<std::string> &Target, const std::string &Value)
{
     const std::string Trimmed = TrimCopy(Value);
     if (Trimmed.empty())
     {
          return;
     }

     const std::string LowerTrimmed = ToLowerCopy(Trimmed);
     for (const auto &Existing : Target)
     {
          if (ToLowerCopy(Existing) == LowerTrimmed)
          {
               return;
          }
     }

     Target.push_back(Trimmed);
}

inline const std::unordered_map<std::string, std::string> &GetCanonicalSubjectMap()
{
     static const std::unordered_map<std::string, std::string> Canonical = {
          {"usa", "united states"},
          {"us", "united states"},
          {"u.s.", "united states"},
          {"u.s.a.", "united states"},
          {"america", "united states"},
          {"uk", "united kingdom"},
          {"u.k.", "united kingdom"}};
     return Canonical;
}

inline std::vector<std::string> SplitWords(const std::string &Value)
{
     std::vector<std::string> Words;
     std::istringstream In(Value);
     std::string Word;
     while (In >> Word)
     {
          Words.push_back(TrimTrailingPunctuation(Word));
     }
     return Words;
}

inline bool IsGenericDescriptor(const std::string &Word)
{
     static const std::unordered_set<std::string> GenericWords = {
          "artist", "artists", "music", "song", "songs", "doc", "docs", "document", "documents",
          "collection", "collections", "female", "male", "good", "best", "top", "nice", "cool",
          "famous", "old", "new", "big", "small", "random", "general", "popular"};
     return GenericWords.find(ToLowerCopy(Word)) != GenericWords.end();
}

inline std::string NormalizeEntitySubject(const std::string &Subject)
{
     const std::string Trimmed = TrimTrailingPunctuation(Subject);
     const std::string Lower = ToLowerCopy(Trimmed);
     const auto &Canonical = GetCanonicalSubjectMap();
     const auto It = Canonical.find(Lower);
     if (It != Canonical.end())
     {
          return It->second;
     }

     return Trimmed;
}

inline std::string ExtractSubjectFromPrefixes(const std::string &Query, const std::vector<std::string> &Prefixes)
{
     const std::string Lower = ToLowerCopy(TrimCopy(Query));
     for (const auto &Prefix : Prefixes)
     {
          if (Lower.rfind(Prefix, 0) == 0)
          {
               return NormalizeEntitySubject(Query.substr(Prefix.size()));
          }
     }

     return "";
}

inline ExclusionPlan ExtractExclusionPlan(const std::string &Query)
{
     ExclusionPlan Plan;
     const std::string LowerQuery = ToLowerCopy(Query);
     const std::vector<std::string> Patterns = {" without ", " excluding ", " except ", " minus ", " not "};

     for (const auto &Pattern : Patterns)
     {
          size_t Pos = LowerQuery.find(Pattern);
          if (Pos == std::string::npos)
          {
               continue;
          }

          std::string Tail = TrimCopy(Query.substr(Pos + Pattern.size()));
          size_t End = Tail.find_first_of(".,;");
          if (End != std::string::npos)
          {
               Tail = Tail.substr(0, End);
          }

          Tail = TrimTrailingPunctuation(Tail);
          if (!Tail.empty())
          {
               AppendUnique(Plan.Terms, Tail);
               Plan.HasExclusions = true;
               Plan.Reason = "user requested exclusion";
          }
     }

     return Plan;
}

inline const std::unordered_map<std::string, std::vector<std::string>> &GetEntityAliasMap()
{
     static const std::unordered_map<std::string, std::vector<std::string>> AliasMap = {
          {"chile", {"Republic of Chile", "Chilean Republic", "Chile country", "Chilean"}},
          {"argentina", {"Argentine Republic", "Argentina country", "Argentine"}},
          {"peru", {"Republic of Peru", "Peru country", "Peruvian"}},
          {"brazil", {"Federative Republic of Brazil", "Brazil country", "Brazilian"}},
          {"mexico", {"United Mexican States", "Mexico country", "Mexican"}},
          {"spain", {"Kingdom of Spain", "Spain country", "Spanish state"}},
          {"france", {"French Republic", "France country", "French state"}},
          {"germany", {"Federal Republic of Germany", "Germany country", "German state"}},
          {"japan", {"Japan country", "State of Japan", "Japanese state"}},
          {"united states", {"USA", "US", "United States of America", "America"}},
          {"the ohio state university", {"Ohio State", "OSU", "Ohio State University", "Ohio State Columbus"}},
          {"university of pennsylvania", {"UPenn", "Penn", "Pennsylvania University", "University of Pennsylvania Philadelphia"}},
          {"pennsylvania state university", {"Penn State", "PSU", "Pennsylvania State", "Penn State University"}},
          {"university of california berkeley", {"UC Berkeley", "Berkeley", "Cal", "California Berkeley"}},
          {"university of michigan ann arbor", {"UMich", "U Mich", "Michigan Ann Arbor", "University of Michigan"}},
          {"university of illinois urbana champaign", {"UIUC", "Illinois Urbana Champaign", "Urbana Champaign", "Illinois UC"}},
          {"georgia institute of technology", {"Georgia Tech", "GaTech", "Georgia Institute Tech"}},
          {"university of florida", {"UF", "Florida Gators University", "Florida University"}},
          {"university of washington", {"UW", "Washington University Seattle"}},
          {"kurt cobain", {"Cobain", "Kurt Donald Cobain", "Nirvana frontman", "Nirvana singer", "lead singer of Nirvana"}},
          {"madonna", {"Madonna Louise Ciccone", "Madonna Ciccone", "Queen of Pop", "Madonna singer"}},
          {"weather", {"climate", "meteorology", "atmospheric science", "climate science", "forecasting"}},
          {"climate", {"weather", "climate science", "atmospheric science", "meteorology"}},
          {"geology", {"earth science", "geoscience", "plate tectonics", "tectonics", "rock science"}},
          {"physics", {"physical science", "relativity", "quantum mechanics", "classical mechanics"}},
          {"biology", {"life science", "genetics", "evolution", "immunology", "gene editing"}},
          {"chemistry", {"chemical science", "periodic table", "elements", "matter science"}},
          {"paint", {"painting", "paintings", "paint art", "painted work"}},
          {"wall paint", {"wall coating", "interior paint", "house paint"}}};
     return AliasMap;
}

inline std::string FindApproximateEntityCanonical(const std::string &Subject)
{
     const std::string Normalized = ToLowerCopy(TrimCopy(Subject));
     if (Normalized.empty())
     {
          return "";
     }

     const auto &AliasMap = GetEntityAliasMap();
     std::string BestCanonical;
     double BestScore = 0.0;

     auto ScoreCandidate = [&](const std::string &Canonical, const std::string &Candidate)
     {
          const std::string CandidateLower = ToLowerCopy(TrimCopy(Candidate));
          if (CandidateLower.empty())
          {
               return;
          }

          const double TokenSet = rapidfuzz::fuzz::token_set_ratio(Normalized, CandidateLower);
          const double TokenSort = rapidfuzz::fuzz::token_sort_ratio(Normalized, CandidateLower);
          const double Partial = rapidfuzz::fuzz::partial_ratio(Normalized, CandidateLower);
          const double Score = std::max(TokenSet, std::max(TokenSort, Partial));

          if (Score > BestScore)
          {
               BestScore = Score;
               BestCanonical = Canonical;
          }
     };

     for (const auto &Entry : AliasMap)
     {
          ScoreCandidate(Entry.first, Entry.first);
          for (const auto &Alias : Entry.second)
          {
               ScoreCandidate(Entry.first, Alias);
          }
     }

     if (BestScore >= 90.0)
     {
          return BestCanonical;
     }

     return "";
}

inline const std::unordered_map<std::string, std::vector<std::string>> &GetEntityRelatedMap()
{
     static const std::unordered_map<std::string, std::vector<std::string>> RelatedMap = {
          {"chile", {"Santiago", "South America", "Andes", "country", "nation"}},
          {"kurt cobain", {"Nirvana", "grunge", "Seattle", "rock", "Nevermind"}},
          {"madonna", {"pop", "singer", "music", "albums", "performer"}},
          {"weather", {"climate", "atmosphere", "meteorology", "temperature", "ocean"}},
          {"climate", {"weather", "atmosphere", "greenhouse", "ocean", "temperature"}},
          {"geology", {"earth", "rocks", "crust", "tectonic plates", "mantle"}},
          {"physics", {"gravity", "spacetime", "motion", "energy", "quantum"}},
          {"biology", {"dna", "genes", "evolution", "crispr", "immunity"}},
          {"chemistry", {"elements", "atoms", "molecules", "periodic table", "reactions"}},
          {"paint", {"color", "pigment", "canvas", "brush", "art"}},
          {"wall paint", {"interior walls", "paint finish", "primer", "latex paint", "wall coating"}}};
     return RelatedMap;
}

inline const std::unordered_map<std::string, std::vector<std::string>> &GetSecondHopExpansionMap()
{
     static const std::unordered_map<std::string, std::vector<std::string>> ExpansionMap = {
          {"united states", {"washington dc", "washington d.c.", "federal government", "north america"}},
          {"washington dc", {"capital city", "government seat", "district of columbia"}},
          {"madonna", {"queen of pop", "pop singer", "music"}},
          {"queen of pop", {"pop singer", "music artist", "albums"}},
          {"kurt cobain", {"nirvana", "grunge", "rock singer"}},
          {"nirvana", {"grunge", "seattle", "rock band"}},
          {"weather", {"climate science", "atmospheric science", "long-term atmospheric changes"}},
          {"climate", {"climate science", "atmospheric changes", "ocean changes"}},
          {"meteorology", {"weather", "atmospheric science", "forecasting"}},
          {"geology", {"plate tectonics", "earth crust", "continental drift"}},
          {"earth science", {"plate tectonics", "rocks", "crust"}},
          {"physics", {"relativity", "quantum mechanics", "spacetime gravity"}},
          {"relativity", {"spacetime", "gravity", "high-speed motion"}},
          {"quantum mechanics", {"quantum", "particles", "wave functions"}},
          {"biology", {"dna and genetics", "evolution by natural selection", "crispr gene editing", "vaccines and immunology"}},
          {"chemistry", {"the periodic table", "chemical elements", "atoms"}},
          {"capital city", {"government seat", "administrative center", "national capital"}},
          {"paint", {"painting", "pigment", "art material"}},
          {"wall paint", {"interior paint", "primer", "latex paint"}}};
     return ExpansionMap;
}

inline const std::unordered_map<std::string, std::string> &GetCapitalFacts()
{
     static const std::unordered_map<std::string, std::string> Facts = {
          {"united states", "Washington, D.C. is the capital of the United States."},
          {"chile", "Santiago is the capital of Chile."},
          {"argentina", "Buenos Aires is the capital of Argentina."},
          {"peru", "Lima is the capital of Peru."},
          {"brazil", "Brasilia is the capital of Brazil."},
          {"france", "Paris is the capital of France."},
          {"japan", "Tokyo is the capital of Japan."}};
     return Facts;
}

inline const std::unordered_map<std::string, std::string> &GetCountryFacts()
{
     static const std::unordered_map<std::string, std::string> Facts = {
          {"santiago", "Santiago is in Chile."},
          {"paris", "Paris is in France."},
          {"tokyo", "Tokyo is in Japan."}};
     return Facts;
}

inline const std::unordered_map<std::string, std::string> &GetEntityFacts()
{
     static const std::unordered_map<std::string, std::string> Facts = {
          {"madonna", "Madonna is an American singer, songwriter, and actress."},
          {"kurt cobain", "Kurt Cobain was an American musician and the frontman of Nirvana."}};
     return Facts;
}

inline EntityAnalysis AnalyzeEntitySubject(const std::string &Subject)
{
     EntityAnalysis Analysis;
     Analysis.NormalizedSubject = ToLowerCopy(NormalizeEntitySubject(Subject));
     if (Analysis.NormalizedSubject.empty())
     {
          return Analysis;
     }

     AppendUnique(Analysis.PossibleSynonyms, NormalizeEntitySubject(Subject));

     const auto &AliasMap = GetEntityAliasMap();
     auto AliasIt = AliasMap.find(Analysis.NormalizedSubject);
     if (AliasIt == AliasMap.end())
     {
          const std::string ApproximateCanonical = FindApproximateEntityCanonical(Analysis.NormalizedSubject);
          if (!ApproximateCanonical.empty())
          {
               Analysis.NormalizedSubject = ApproximateCanonical;
               AliasIt = AliasMap.find(Analysis.NormalizedSubject);
               AppendUnique(Analysis.PossibleSynonyms, ApproximateCanonical);
          }
     }
     if (AliasIt != AliasMap.end())
     {
          for (const auto &Alias : AliasIt->second)
          {
               AppendUnique(Analysis.PossibleSynonyms, Alias);
          }
     }

     const auto &RelatedMap = GetEntityRelatedMap();
     const auto RelatedIt = RelatedMap.find(Analysis.NormalizedSubject);
     if (RelatedIt != RelatedMap.end())
     {
          for (const auto &Term : RelatedIt->second)
          {
               AppendUnique(Analysis.RelatedTerms, Term);
          }
     }

     if (Analysis.NormalizedSubject == "madonna" || Analysis.NormalizedSubject == "kurt cobain")
     {
          Analysis.EntityType = "person";
     }
     else if (AliasIt != AliasMap.end())
     {
          Analysis.EntityType = "country_or_entity";
     }

     return Analysis;
}

inline std::string LookupBuiltinFact(const std::string &QuestionType, const std::string &NormalizedSubject)
{
     if (NormalizedSubject.empty())
     {
    return "";
}

     if (QuestionType == "capital_lookup")
     {
          const auto &Facts = GetCapitalFacts();
          const auto It = Facts.find(NormalizedSubject);
          return It == Facts.end() ? "" : It->second;
     }

     if (QuestionType == "country_lookup")
     {
          const auto &Facts = GetCountryFacts();
          const auto It = Facts.find(NormalizedSubject);
          return It == Facts.end() ? "" : It->second;
     }

     if (QuestionType == "who_is_lookup" || QuestionType == "who_was_lookup" || QuestionType == "what_is_lookup" || QuestionType == "about_lookup")
     {
          const auto &Facts = GetEntityFacts();
          const auto It = Facts.find(NormalizedSubject);
          return It == Facts.end() ? "" : It->second;
     }

     return "";
}

inline std::unordered_map<std::string, std::vector<std::string>> GetExpandedFactMap()
{
     static std::unordered_map<std::string, std::vector<std::string>> Facts = {
          {"capital_lookup", {"washington dc", "santiago", "paris", "tokyo"}},
          {"country_lookup", {"united states", "chile", "france", "japan"}},
          {"who_is_lookup", {"madonna", "kurt cobain"}},
          {"entity_lookup", {"pop singer", "grunge", "rock"}}};
     return Facts;
}

inline QueryRewritePlan BuildQueryRewritePlan(const std::string &QuestionType,
                                              const std::string &Subject,
                                              const EntityAnalysis &Analysis)
{
     QueryRewritePlan Plan;
     const std::string CanonicalSubject = NormalizeEntitySubject(Subject);

     if (!CanonicalSubject.empty())
     {
          AppendUnique(Plan.RewritePhrases, CanonicalSubject);
          Plan.RewriteReasons.push_back("normalized canonical subject");
     }

     for (const auto &Alias : Analysis.PossibleSynonyms)
     {
          AppendUnique(Plan.RewritePhrases, Alias);
     }

     for (const auto &Term : Analysis.RelatedTerms)
     {
          AppendUnique(Plan.RewritePhrases, Term);
     }

     if (QuestionType == "capital_lookup" && !CanonicalSubject.empty())
     {
          AppendUnique(Plan.RewritePhrases, "capital city " + CanonicalSubject);
          AppendUnique(Plan.RewritePhrases, CanonicalSubject + " government seat");
          Plan.RewriteReasons.push_back("capital lookup rewrite");
     }
     else if ((QuestionType == "who_is_lookup" || QuestionType == "who_was_lookup") && !CanonicalSubject.empty())
     {
          AppendUnique(Plan.RewritePhrases, CanonicalSubject + " biography");
          AppendUnique(Plan.RewritePhrases, CanonicalSubject + " career");
          Plan.RewriteReasons.push_back("person lookup rewrite");
     }
     else if ((QuestionType == "what_is_lookup" || QuestionType == "about_lookup") && !CanonicalSubject.empty())
     {
          AppendUnique(Plan.RewritePhrases, CanonicalSubject + " overview");
          AppendUnique(Plan.RewritePhrases, CanonicalSubject + " definition");
          Plan.RewriteReasons.push_back("entity overview rewrite");
     }
     else if (!CanonicalSubject.empty() && Analysis.EntityType == "person")
     {
          AppendUnique(Plan.RewritePhrases, CanonicalSubject + " songs");
          AppendUnique(Plan.RewritePhrases, CanonicalSubject + " albums");
          Plan.RewriteReasons.push_back("person/entity retrieval rewrite");
     }

     const auto &SecondHopMap = GetSecondHopExpansionMap();
     std::vector<std::string> FirstHop = Plan.RewritePhrases;
     for (const auto &Phrase : FirstHop)
     {
          const auto It = SecondHopMap.find(ToLowerCopy(Phrase));
          if (It == SecondHopMap.end())
          {
               continue;
          }

          for (const auto &Expansion : It->second)
          {
               AppendUnique(Plan.RewritePhrases, Expansion);
          }
     }

     if (!Plan.RewritePhrases.empty())
     {
          Plan.RewriteReasons.push_back("multi_hop entity expansion");
     }

     return Plan;
}

inline VagueQueryPlan BuildVagueQueryPlan(const std::string &Query,
                                          const std::string &Topic,
                                          const std::string &Where,
                                          const EntityAnalysis &Analysis,
                                          const QueryRewritePlan &RewritePlan)
{
     VagueQueryPlan Plan;
     const std::vector<std::string> Words = SplitWords(Query);
     int SpecificCount = 0;

     for (const auto &Word : Words)
     {
          if (!Word.empty() && !IsGenericDescriptor(Word))
          {
               ++SpecificCount;
          }
     }

     const bool ShortGenericQuery = Words.size() <= 3 && SpecificCount == 0;
     const bool CollectionOnlyShape = !Where.empty() && Topic.empty() && SpecificCount <= 1;
     const bool GenericTopic = !Topic.empty() && SplitWords(Topic).size() <= 2 && SpecificCount <= 1;

     if (ShortGenericQuery)
     {
          Plan.IsVague = true;
          Plan.Reason = "query_is_short_and_generic";
     }
     else if (CollectionOnlyShape)
     {
          Plan.IsVague = true;
          Plan.Reason = "collection_scope_without_specific_topic";
     }
     else if (GenericTopic)
     {
          Plan.IsVague = true;
          Plan.Reason = "topic_is_generic";
     }

     if (!Plan.IsVague)
     {
          return Plan;
     }

     static const std::unordered_map<std::string, std::vector<std::string>> GenericExpansions = {
          {"artists", {"singers", "bands", "musicians", "performers"}},
          {"artist", {"singer", "band", "musician", "performer"}},
          {"music", {"songs", "albums", "bands", "artists"}},
          {"paint", {"painting", "wall paint", "art paint", "pigment"}},
          {"capital", {"capital city", "government seat", "national capital"}}};

     for (const auto &Word : Words)
     {
          const auto It = GenericExpansions.find(ToLowerCopy(Word));
          if (It == GenericExpansions.end())
          {
               continue;
          }

          for (const auto &Term : It->second)
          {
               AppendUnique(Plan.ClarifyingTerms, Term);
          }
     }

     for (const auto &Synonym : Analysis.PossibleSynonyms)
     {
          AppendUnique(Plan.ClarifyingTerms, Synonym);
     }
     for (const auto &Term : Analysis.RelatedTerms)
     {
          AppendUnique(Plan.ClarifyingTerms, Term);
     }

     for (const auto &Rewrite : RewritePlan.RewritePhrases)
     {
          AppendUnique(Plan.SuggestedQueries, Rewrite);
     }

     if (!Where.empty())
     {
          for (const auto &Term : Plan.ClarifyingTerms)
          {
               AppendUnique(Plan.SuggestedQueries, Term + " in collection " + Where);
          }
          AppendUnique(Plan.FollowUpQuestions, "Do you want to search only in " + Where + "?");
     }
     else
     {
          AppendUnique(Plan.FollowUpQuestions, "Which collection should I search?");
     }

     if (!Topic.empty())
     {
          AppendUnique(Plan.FollowUpQuestions, "Do you mean " + Topic + " in a specific category or collection?");
     }
     else if (!Plan.ClarifyingTerms.empty())
     {
          AppendUnique(Plan.FollowUpQuestions, "Do you mean " + Plan.ClarifyingTerms.front() + "?");
     }

     return Plan;
}
}
