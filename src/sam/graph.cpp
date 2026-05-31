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

#include <algorithm>
#include <cctype>
#include <ctime>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "sam/internal.h"

struct IntentGraphEdgeAccumulator
{
     std::string Text;
     std::string Kind;
     double Weight = 0.0;
     size_t Support = 0;
};

struct IntentGraphNodeAccumulator
{
     std::string Text;
     std::string Kind;
     double Weight = 0.0;
     size_t Support = 0;
     std::unordered_map<std::string, double> DocScores;
     std::unordered_map<std::string, IntentGraphEdgeAccumulator> Edges;
};

struct IntentGraphNodeMatch
{
     std::string Text;
     std::string Kind;
     double Score = 0.0;
     size_t Support = 0;
     std::vector<std::string> Docs;
     std::vector<std::string> Neighbors;
};

std::string TrimIntentValue(const std::string& Value)
{
     const size_t Start = Value.find_first_not_of(" \t\r\n");

     if (Start == std::string::npos)
     {
          return "";
     }

     const size_t End = Value.find_last_not_of(" \t\r\n");
     return Value.substr(Start, End - Start + 1);
}

std::string NormalizeIntentText(const std::string& Value)
{
     std::string Normalized;
     Normalized.reserve(Value.size());
     bool LastWasSpace = false;

     for (unsigned char C : Value)
     {
          if (std::isalnum(C))
          {
               Normalized.push_back(static_cast<char>(std::tolower(C)));
               LastWasSpace = false;
          }
          else if (std::isspace(C) || C == '-' || C == '_' || C == '/' || C == '.')
          {
               if (!Normalized.empty() && !LastWasSpace)
               {
                    Normalized.push_back(' ');
                    LastWasSpace = true;
               }
          }
     }

     return TrimIntentValue(Normalized);
}

std::vector<std::string> TokenizeIntentText(const std::string& Value)
{
     std::vector<std::string> Tokens;
     std::istringstream Input(Value);
     std::string Token;

     while (Input >> Token)
     {
          Tokens.push_back(Token);
     }

     return Tokens;
}

bool ShouldKeepIntentPhrase(const std::string& Value)
{
     if (Value.size() < 3)
     {
          return false;
     }

     const std::vector<std::string> Tokens = TokenizeIntentText(Value);

     if (Tokens.empty())
     {
          return false;
     }

     size_t StrongTokens = 0;

     for (const std::string& Token : Tokens)
     {
          if (Token.size() >= 3)
          {
               ++StrongTokens;
          }
     }

     return StrongTokens > 0;
}

double ClampIntentScore(double Value)
{
     return std::max(0.0, std::min(1.0, Value));
}

void InsertRankedString(std::vector<std::string>& Values,
                        std::unordered_set<std::string>& Seen,
                        const std::string& Candidate,
                        size_t Limit)
{
     if (Candidate.empty() || !Seen.insert(Candidate).second)
     {
          return;
     }

     Values.push_back(Candidate);

     if (Values.size() > Limit)
     {
          Values.resize(Limit);
     }
}

void AttachIntentDoc(IntentGraphNodeAccumulator& Node,
                     const std::string& DocumentID,
                     double Score)
{
     if (DocumentID.empty() || Score <= 0.0)
     {
          return;
     }

     double& Existing = Node.DocScores[DocumentID];
     Existing = std::max(Existing, Score);
}

void AttachIntentEdge(IntentGraphNodeAccumulator& Node,
                      const std::string& Neighbor,
                      const std::string& Kind,
                      double Weight)
{
     if (Neighbor.empty() || Neighbor == Node.Text || Weight <= 0.0)
     {
          return;
     }

     IntentGraphEdgeAccumulator& Edge = Node.Edges[Neighbor];
     Edge.Text = Neighbor;
     Edge.Kind = Kind;
     Edge.Weight += Weight;
     ++Edge.Support;
}

IntentGraphNodeAccumulator& EnsureIntentNode(std::unordered_map<std::string, IntentGraphNodeAccumulator>& Nodes,
                                             const std::string& Text,
                                             const std::string& Kind,
                                             double Weight,
                                             size_t Support,
                                             const std::string& DocumentID)
{
     IntentGraphNodeAccumulator& Node = Nodes[Text];

     if (Node.Text.empty())
     {
          Node.Text = Text;
          Node.Kind = Kind;
     }

     if (Node.Kind.empty() || Node.Kind == "term")
     {
          Node.Kind = Kind;
     }

     Node.Weight += Weight;
     Node.Support += Support;
     AttachIntentDoc(Node, DocumentID, Weight);
     return Node;
}

void AddIntentNode(std::unordered_map<std::string, IntentGraphNodeAccumulator>& Nodes,
                   const std::string& Text,
                   const std::string& Kind,
                   double Weight,
                   size_t Support,
                   const std::string& DocumentID,
                   const std::vector<std::pair<std::string, std::pair<std::string, double>>>& Neighbors = {})
{
     const std::string Normalized = NormalizeIntentText(Text);

     if (!ShouldKeepIntentPhrase(Normalized))
     {
          return;
     }

     IntentGraphNodeAccumulator& Node =
          EnsureIntentNode(Nodes, Normalized, Kind, Weight, std::max<size_t>(1, Support), DocumentID);

     for (const auto& Neighbor : Neighbors)
     {
          const std::string NeighborText = NormalizeIntentText(Neighbor.first);

          if (!ShouldKeepIntentPhrase(NeighborText))
          {
               continue;
          }

          AttachIntentEdge(Node, NeighborText, Neighbor.second.first, Neighbor.second.second);
     }
}

double ComputeIntentTextOverlap(const std::vector<std::string>& QueryTokens,
                                const std::vector<std::string>& CandidateTokens)
{
     if (QueryTokens.empty() || CandidateTokens.empty())
     {
          return 0.0;
     }

     std::unordered_set<std::string> CandidateSet(CandidateTokens.begin(), CandidateTokens.end());
     size_t Matches = 0;

     for (const std::string& Token : QueryTokens)
     {
          if (CandidateSet.find(Token) != CandidateSet.end())
          {
               ++Matches;
          }
     }

     return static_cast<double>(Matches) /
            static_cast<double>(std::max<size_t>(QueryTokens.size(), CandidateTokens.size()));
}

std::vector<IntentGraphNodeMatch> LoadIntentGraphMatches(rocksdb::DB* Database,
                                                         const std::string& Collection,
                                                         const std::string& Query)
{
     std::vector<IntentGraphNodeMatch> Matches;

     if (!Database || Collection.empty())
     {
          return Matches;
     }

     const std::string NormalizedQuery = NormalizeIntentText(Query);
     const std::vector<std::string> QueryTokens = TokenizeIntentText(NormalizedQuery);

     if (NormalizedQuery.empty() || QueryTokens.empty())
     {
          return Matches;
     }

     std::string RawGraph;
     const rocksdb::Status Status =
          Database->Get(rocksdb::ReadOptions(), BuildIntentGraphKey(Collection), &RawGraph);

     if (!Status.ok() || RawGraph.empty())
     {
          return Matches;
     }

     try
     {
          const nlohmann::json Root = nlohmann::json::parse(RawGraph);

          if (!Root.contains("nodes") || !Root["nodes"].is_array())
          {
               return Matches;
          }

          for (const auto& NodeItem : Root["nodes"])
          {
               const std::string Text = NormalizeIntentText(NodeItem.value("text", ""));

               if (Text.empty())
               {
                    continue;
               }

               const std::vector<std::string> CandidateTokens = TokenizeIntentText(Text);
               double MatchScore = ComputeIntentTextOverlap(QueryTokens, CandidateTokens);

               if (Text == NormalizedQuery)
               {
                    MatchScore = 1.0;
               }
               else if (Text.find(NormalizedQuery) != std::string::npos ||
                        NormalizedQuery.find(Text) != std::string::npos)
               {
                    MatchScore = std::max(MatchScore, 0.84);
               }

               if (MatchScore < 0.28)
               {
                    continue;
               }

               IntentGraphNodeMatch Match;
               Match.Text = Text;
               Match.Kind = NodeItem.value("kind", "term");
               Match.Score = ClampIntentScore((MatchScore * 0.72) +
                                              (ClampIntentScore(NodeItem.value("weight", 0.0)) * 0.28));
               Match.Support = NodeItem.value("support", 0U);

               std::unordered_set<std::string> SeenDocs;

               if (NodeItem.contains("docs") && NodeItem["docs"].is_array())
               {
                    for (const auto& DocItem : NodeItem["docs"])
                    {
                         if (!DocItem.is_string())
                         {
                              continue;
                         }

                         InsertRankedString(Match.Docs, SeenDocs, DocItem.get<std::string>(), 8);
                    }
               }

               std::unordered_set<std::string> SeenNeighbors;

               if (NodeItem.contains("neighbors") && NodeItem["neighbors"].is_array())
               {
                    for (const auto& NeighborItem : NodeItem["neighbors"])
                    {
                         if (!NeighborItem.is_object())
                         {
                              continue;
                         }

                         InsertRankedString(Match.Neighbors,
                                            SeenNeighbors,
                                            NormalizeIntentText(NeighborItem.value("text", "")),
                                            10);
                    }
               }

               Matches.push_back(std::move(Match));
          }

          std::sort(Matches.begin(), Matches.end(),
                    [](const IntentGraphNodeMatch& A, const IntentGraphNodeMatch& B)
                    {
                         if (A.Score != B.Score)
                         {
                              return A.Score > B.Score;
                         }

                         if (A.Support != B.Support)
                         {
                              return A.Support > B.Support;
                         }

                         return A.Text < B.Text;
                    });
     }
     catch (...)
     {
     }

     return Matches;
}

std::string BuildIntentGraphKey(const std::string& Collection)
{
     return "sam:graph:" + Collection;
}

bool RebuildIntentGraphLocked(rocksdb::DB* Database,
                              const std::string& Collection,
                              std::string* ErrorMessage)
{
     if (!Database || Collection.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Intent graph rebuild requires an open database and collection.";
          }

          return false;
     }

     const std::string ManifestPrefix = "sam:doc:" + Collection + ":";
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     std::unordered_map<std::string, IntentGraphNodeAccumulator> Nodes;
     size_t DocumentCount = 0;

     for (Iterator->Seek(ManifestPrefix);
          Iterator->Valid() && Iterator->key().starts_with(ManifestPrefix);
          Iterator->Next())
     {
          SAM::DocumentEntry Entry;

          if (!ParseManifestValue(Iterator->value().ToString(), Entry))
          {
               continue;
          }

          ++DocumentCount;
          const std::string DocumentID = Entry.DocumentID;
          const std::string Subject = NormalizeIntentText(Entry.Subject.empty() ? Entry.Title : Entry.Subject);
          const std::string Title = NormalizeIntentText(Entry.Title);
          const std::string Summary = NormalizeIntentText(Entry.Summary);

          if (ShouldKeepIntentPhrase(Subject))
          {
               std::vector<std::pair<std::string, std::pair<std::string, double>>> SubjectNeighbors;

               if (ShouldKeepIntentPhrase(Title) && Title != Subject)
               {
                    SubjectNeighbors.push_back({Title, {"title", 0.46}});
               }

               if (ShouldKeepIntentPhrase(Summary))
               {
                    SubjectNeighbors.push_back({Summary, {"summary", 0.24}});
               }

               AddIntentNode(Nodes, Subject, "subject", 0.94, 2, DocumentID, SubjectNeighbors);
          }

          if (ShouldKeepIntentPhrase(Title))
          {
               std::vector<std::pair<std::string, std::pair<std::string, double>>> TitleNeighbors;

               if (ShouldKeepIntentPhrase(Subject) && Subject != Title)
               {
                    TitleNeighbors.push_back({Subject, {"subject", 0.52}});
               }

               AddIntentNode(Nodes, Title, "title", 0.72, 1, DocumentID, TitleNeighbors);
          }

          for (const std::string& Alias : Entry.Aliases)
          {
               const std::string NormalizedAlias = NormalizeIntentText(Alias);

               if (!ShouldKeepIntentPhrase(NormalizedAlias))
               {
                    continue;
               }

               AddIntentNode(Nodes,
                             NormalizedAlias,
                             "alias",
                             0.68,
                             1,
                             DocumentID,
                             {{Subject, {"subject", 0.58}},
                              {Title, {"title", 0.44}}});
          }

          for (const std::string& Descriptor : Entry.Descriptors)
          {
               const std::string NormalizedDescriptor = NormalizeIntentText(Descriptor);

               if (!ShouldKeepIntentPhrase(NormalizedDescriptor))
               {
                    continue;
               }

               AddIntentNode(Nodes,
                             NormalizedDescriptor,
                             "descriptor",
                             0.52,
                             1,
                             DocumentID,
                             {{Subject, {"subject", 0.46}},
                              {Title, {"title", 0.34}}});
          }

          for (const std::string& Query : Entry.Queries)
          {
               const std::string NormalizedQuery = NormalizeIntentText(Query);

               if (!ShouldKeepIntentPhrase(NormalizedQuery))
               {
                    continue;
               }

               AddIntentNode(Nodes,
                             NormalizedQuery,
                             "query",
                             0.74,
                             1,
                             DocumentID,
                             {{Subject, {"subject", 0.64}},
                              {Title, {"title", 0.40}},
                              {Summary, {"summary", 0.24}}});
          }

          for (const auto& Term : Entry.Terms)
          {
               const std::string NormalizedTerm = NormalizeIntentText(Term.Text);

               if (!ShouldKeepIntentPhrase(NormalizedTerm))
               {
                    continue;
               }

               AddIntentNode(Nodes,
                             NormalizedTerm,
                             Term.Kind.empty() ? "term" : Term.Kind,
                             0.34 + (ClampIntentScore(Term.Score) * 0.34) +
                                  (ClampIntentScore(Term.Signal) * 0.18),
                             1,
                             DocumentID,
                             {{Subject, {"subject", 0.30}},
                              {Title, {"title", 0.20}}});
          }
     }

     std::string RawProfile;
     const rocksdb::Status ProfileStatus =
          Database->Get(rocksdb::ReadOptions(), BuildCollectionProfileKey(Collection), &RawProfile);

     if (ProfileStatus.ok() && !RawProfile.empty())
     {
          try
          {
               const nlohmann::json Profile = nlohmann::json::parse(RawProfile);

               auto AttachProfileTerms = [&](const char* Key, const char* KindBase, double WeightBias)
               {
                    if (!Profile.contains(Key) || !Profile[Key].is_array())
                    {
                         return;
                    }

                    for (const auto& Item : Profile[Key])
                    {
                         const std::string Text = NormalizeIntentText(Item.value("text", ""));

                         if (!ShouldKeepIntentPhrase(Text))
                         {
                              continue;
                         }

                         std::vector<std::pair<std::string, std::pair<std::string, double>>> Neighbors;

                         if (Item.contains("related") && Item["related"].is_array())
                         {
                              for (const auto& Related : Item["related"])
                              {
                                   if (!Related.is_string())
                                   {
                                        continue;
                                   }

                                   Neighbors.push_back({NormalizeIntentText(Related.get<std::string>()),
                                                        {"related", 0.34}});
                              }
                         }

                         AddIntentNode(Nodes,
                                       Text,
                                       KindBase,
                                       WeightBias + (ClampIntentScore(Item.value("score", 0.0)) * 0.42),
                                       std::max<size_t>(1, Item.value("support", 1U)),
                                       "",
                                       Neighbors);
                    }
               };

               auto AttachProfileFamilies = [&](const char* Key, const char* KindBase, double WeightBias)
               {
                    if (!Profile.contains(Key) || !Profile[Key].is_array())
                    {
                         return;
                    }

                    for (const auto& Item : Profile[Key])
                    {
                         const std::string Subject = NormalizeIntentText(Item.value("subject", ""));

                         if (!ShouldKeepIntentPhrase(Subject))
                         {
                              continue;
                         }

                         std::vector<std::pair<std::string, std::pair<std::string, double>>> SubjectNeighbors;

                         if (Item.contains("aliases") && Item["aliases"].is_array())
                         {
                              for (const auto& Alias : Item["aliases"])
                              {
                                   if (!Alias.is_string())
                                   {
                                        continue;
                                   }

                                   SubjectNeighbors.push_back({NormalizeIntentText(Alias.get<std::string>()),
                                                               {"alias", 0.48}});
                              }
                         }

                         if (Item.contains("descriptors") && Item["descriptors"].is_array())
                         {
                              for (const auto& Descriptor : Item["descriptors"])
                              {
                                   if (!Descriptor.is_string())
                                   {
                                        continue;
                                   }

                                   SubjectNeighbors.push_back({NormalizeIntentText(Descriptor.get<std::string>()),
                                                               {"descriptor", 0.34}});
                              }
                         }

                         if (Item.contains("queries") && Item["queries"].is_array())
                         {
                              for (const auto& Query : Item["queries"])
                              {
                                   if (!Query.is_string())
                                   {
                                        continue;
                                   }

                                   SubjectNeighbors.push_back({NormalizeIntentText(Query.get<std::string>()),
                                                               {"query", 0.42}});
                              }
                         }

                         AddIntentNode(Nodes,
                                       Subject,
                                       KindBase,
                                       WeightBias + (ClampIntentScore(Item.value("score", 0.0)) * 0.50),
                                       std::max<size_t>(1, Item.value("support", 1U)),
                                       "",
                                       SubjectNeighbors);
                    }
               };

               AttachProfileTerms("terms", "profile_term", 0.32);
               AttachProfileTerms("learned_terms", "learned_term", 0.42);
               AttachProfileFamilies("families", "profile_subject", 0.52);
               AttachProfileFamilies("learned_families", "learned_subject", 0.62);
          }
          catch (...)
          {
          }
     }

     std::vector<nlohmann::json> NodeValues;
     NodeValues.reserve(Nodes.size());

     for (auto& Pair : Nodes)
     {
          IntentGraphNodeAccumulator& Node = Pair.second;

          if (Node.Text.empty() || Node.Support == 0)
          {
               continue;
          }

          std::vector<std::pair<std::string, double>> RankedDocs(Node.DocScores.begin(), Node.DocScores.end());
          std::sort(RankedDocs.begin(), RankedDocs.end(),
                    [](const auto& A, const auto& B)
                    {
                         if (A.second != B.second)
                         {
                              return A.second > B.second;
                         }

                         return A.first < B.first;
                    });

          std::vector<nlohmann::json> NeighborValues;
          std::vector<IntentGraphEdgeAccumulator> RankedNeighbors;
          RankedNeighbors.reserve(Node.Edges.size());

          for (auto& EdgePair : Node.Edges)
          {
               RankedNeighbors.push_back(EdgePair.second);
          }

          std::sort(RankedNeighbors.begin(), RankedNeighbors.end(),
                    [](const IntentGraphEdgeAccumulator& A, const IntentGraphEdgeAccumulator& B)
                    {
                         if (A.Weight != B.Weight)
                         {
                              return A.Weight > B.Weight;
                         }

                         if (A.Support != B.Support)
                         {
                              return A.Support > B.Support;
                         }

                         return A.Text < B.Text;
                    });

          if (RankedNeighbors.size() > 10)
          {
               RankedNeighbors.resize(10);
          }

          for (const IntentGraphEdgeAccumulator& Edge : RankedNeighbors)
          {
               NeighborValues.push_back({
                    {"text", Edge.Text},
                    {"kind", Edge.Kind},
                    {"weight", ClampIntentScore(Edge.Weight / static_cast<double>(std::max<size_t>(1, Edge.Support)))},
                    {"support", Edge.Support}
               });
          }

          nlohmann::json NodeValue;
          NodeValue["text"] = Node.Text;
          NodeValue["kind"] = Node.Kind;
          NodeValue["weight"] = ClampIntentScore(Node.Weight / static_cast<double>(std::max<size_t>(1, Node.Support)));
          NodeValue["support"] = Node.Support;
          NodeValue["docs"] = nlohmann::json::array();
          NodeValue["neighbors"] = NeighborValues;

          for (size_t Index = 0; Index < RankedDocs.size() && Index < 8; ++Index)
          {
               NodeValue["docs"].push_back(RankedDocs[Index].first);
          }

          NodeValues.push_back(std::move(NodeValue));
     }

     std::sort(NodeValues.begin(), NodeValues.end(),
               [](const nlohmann::json& A, const nlohmann::json& B)
               {
                    const size_t ASupport = A.value("support", 0U);
                    const size_t BSupport = B.value("support", 0U);

                    if (ASupport != BSupport)
                    {
                         return ASupport > BSupport;
                    }

                    const double AWeight = A.value("weight", 0.0);
                    const double BWeight = B.value("weight", 0.0);

                    if (AWeight != BWeight)
                    {
                         return AWeight > BWeight;
                    }

                    return A.value("text", "") < B.value("text", "");
               });

     if (NodeValues.size() > 160)
     {
          NodeValues.resize(160);
     }

     nlohmann::json Root;
     Root["collection"] = Collection;
     Root["documents"] = DocumentCount;
     Root["generated_at_ms"] = static_cast<uint64_t>(time(nullptr)) * 1000ULL;
     Root["nodes"] = std::move(NodeValues);

     const rocksdb::Status GraphStatus =
          Database->Put(rocksdb::WriteOptions(), BuildIntentGraphKey(Collection), Root.dump());

     if (!GraphStatus.ok())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = GraphStatus.ToString();
          }

          return false;
     }

     return true;
}

std::vector<std::string> BuildIntentGraphVariants(rocksdb::DB* Database,
                                                  const std::string& Collection,
                                                  const std::string& Query,
                                                  size_t MaxVariants)
{
     std::vector<std::string> Variants;

     if (MaxVariants == 0)
     {
          return Variants;
     }

     const std::vector<IntentGraphNodeMatch> Matches = LoadIntentGraphMatches(Database, Collection, Query);
     std::unordered_set<std::string> Seen;

     for (const IntentGraphNodeMatch& Match : Matches)
     {
          if (Match.Score < 0.34)
          {
               continue;
          }

          for (const std::string& Neighbor : Match.Neighbors)
          {
               if (Neighbor.empty() || Neighbor == Match.Text)
               {
                    continue;
               }

               InsertRankedString(Variants, Seen, Neighbor, MaxVariants);

               if (Variants.size() >= MaxVariants)
               {
                    return Variants;
               }
          }

          if (Match.Kind == "subject" || Match.Kind == "profile_subject" || Match.Kind == "learned_subject")
          {
               InsertRankedString(Variants, Seen, Match.Text, MaxVariants);
          }

          if (Variants.size() >= MaxVariants)
          {
               break;
          }
     }

     return Variants;
}

std::vector<SAM::LookupHit> BuildIntentGraphHits(rocksdb::DB* Database,
                                                 const std::string& Collection,
                                                 const std::string& Query,
                                                 size_t Limit)
{
     std::vector<SAM::LookupHit> Hits;

     if (Limit == 0)
     {
          return Hits;
     }

     const std::vector<IntentGraphNodeMatch> Matches = LoadIntentGraphMatches(Database, Collection, Query);
     std::unordered_map<std::string, SAM::LookupHit> BestByDocument;

     for (const IntentGraphNodeMatch& Match : Matches)
     {
          if (Match.Score < 0.32)
          {
               continue;
          }

          const double SupportBoost = std::min(0.18, static_cast<double>(Match.Support) * 0.01);
          const double NodeScore = ClampIntentScore(Match.Score + SupportBoost);

          for (const std::string& DocumentID : Match.Docs)
          {
               if (DocumentID.empty())
               {
                    continue;
               }

               std::string ManifestValue;
               const rocksdb::Status Status =
                    Database->Get(rocksdb::ReadOptions(),
                                  BuildDocManifestKey(Collection, DocumentID),
                                  &ManifestValue);

               if (!Status.ok())
               {
                    continue;
               }

               SAM::DocumentEntry Entry;

               if (!ParseManifestValue(ManifestValue, Entry))
               {
                    continue;
               }

               SAM::LookupHit Candidate;
               Candidate.Collection = Collection;
               Candidate.DocumentID = DocumentID;
               Candidate.Title = Entry.Title;
               Candidate.MatchedTerm = Match.Text;
               Candidate.MatchedKind = Match.Kind;
               Candidate.MatchedSource = "intent_graph";
               Candidate.MatchedPath = "intent_graph";
               Candidate.TermOrigin = Candidate.MatchedSource;
               Candidate.MatchedSignal = NodeScore;
               Candidate.MatchedScore = std::max(0.48, NodeScore);
               Candidate.EvidenceCount = std::max<size_t>(1, Match.Neighbors.size());
               Candidate.Breakdown.TermScore = Candidate.MatchedScore * 0.56;
               Candidate.Breakdown.SemanticScore = Candidate.MatchedScore * 0.44;
               Candidate.Breakdown.SemanticBonus = std::min(0.12, static_cast<double>(Match.Neighbors.size()) * 0.01);
               Candidate.Breakdown.FinalScore =
                    ClampIntentScore(Candidate.Breakdown.TermScore +
                                     Candidate.Breakdown.SemanticScore +
                                     Candidate.Breakdown.SemanticBonus);
               Candidate.MatchedScore = Candidate.Breakdown.FinalScore;
               Candidate.Explain = "intent_graph node=" + Match.Text + " support=" +
                                   std::to_string(Match.Support);

               const auto Existing = BestByDocument.find(DocumentID);

               if (Existing == BestByDocument.end() ||
                   Existing->second.Breakdown.FinalScore < Candidate.Breakdown.FinalScore)
               {
                    BestByDocument[DocumentID] = std::move(Candidate);
               }
          }
     }

     for (auto& Pair : BestByDocument)
     {
          Hits.push_back(std::move(Pair.second));
     }

     std::sort(Hits.begin(), Hits.end(),
               [](const SAM::LookupHit& A, const SAM::LookupHit& B)
               {
                    if (A.Breakdown.FinalScore != B.Breakdown.FinalScore)
                    {
                         return A.Breakdown.FinalScore > B.Breakdown.FinalScore;
                    }

                    return A.DocumentID < B.DocumentID;
               });

     if (Hits.size() > Limit)
     {
          Hits.resize(Limit);
     }

     return Hits;
}
