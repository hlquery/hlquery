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


#ifdef HLQUERY_SAM_SPLIT_INCLUDE

/* Term extraction, document-term expansion, and LLM/profile term helpers. */

/* Detect tokens that are only numeric characters. */

bool IsNumericLikeSAMToken(const std::string& Token)
{
     return !Token.empty() &&
            std::all_of(Token.begin(), Token.end(),
                        [](unsigned char C)
                        {
                             return std::isdigit(C);
                        });
}

/* Reject identifier-like values that are poor natural-language lookup terms. */

bool IsIdentifierLikeSAMValue(const std::string& Value)
{
     if (Value.empty())
     {
          return true;
     }

     size_t AlphaCount = 0;
     size_t DigitCount = 0;
     size_t SeparatorCount = 0;

     for (unsigned char C : Value)
     {
          if (std::isalpha(C))
          {
               ++AlphaCount;
          }
          else if (std::isdigit(C))
          {
               ++DigitCount;
          }
          else if (C == '-' || C == '_' || C == '/' || C == '.' || C == ':')
          {
               ++SeparatorCount;
          }
     }

     if (Value.find("://") != std::string::npos)
     {
          return true;
     }

     if (Value.find('@') != std::string::npos)
     {
          return true;
     }

     if (Value.find(' ') == std::string::npos &&
         DigitCount > 0 &&
         SeparatorCount > 0 &&
         AlphaCount <= DigitCount + 2)
     {
          return true;
     }

     return false;
}

bool IsTitleLikeSAMField(const std::string& LowerKey)
{
     return LowerKey == "title" || LowerKey == "name" || LowerKey == "headline" ||
            LowerKey == "subject" || LowerKey == "heading";
}

bool IsSummaryLikeSAMField(const std::string& LowerKey)
{
     return LowerKey == "summary" || LowerKey == "description" || LowerKey == "excerpt" ||
            LowerKey == "overview" || LowerKey == "abstract" || LowerKey == "subtitle";
}

bool IsBodyLikeSAMField(const std::string& LowerKey)
{
     return LowerKey == "content" || LowerKey == "body" || LowerKey == "text" ||
            LowerKey == "article" || LowerKey == "markdown" || LowerKey == "notes";
}

bool IsAliasLikeSAMField(const std::string& LowerKey)
{
     return LowerKey == "alias" || LowerKey == "aliases" || LowerKey == "slug" ||
            LowerKey == "handle" || LowerKey == "username" || LowerKey == "short_name" ||
            LowerKey == "nickname";
}

bool IsTaxonomyLikeSAMField(const std::string& LowerKey)
{
     return LowerKey == "tag" || LowerKey == "tags" || LowerKey == "label" ||
            LowerKey == "labels" || LowerKey == "category" || LowerKey == "categories" ||
            LowerKey == "topic" || LowerKey == "topics" || LowerKey == "genre" ||
            LowerKey == "genres" || LowerKey == "brand" || LowerKey == "brands" ||
            LowerKey == "author" || LowerKey == "authors" || LowerKey == "type";
}

bool IsQueryLikeSAMField(const std::string& LowerKey)
{
     return LowerKey == "query" || LowerKey == "queries" || LowerKey == "keywords" ||
            LowerKey == "search_terms";
}

std::vector<std::string> ExtractArrayishSAMValues(const std::string& RawValue,
                                                  size_t MaxValues = 16)
{
     std::vector<std::string> Values;
     const std::string Trimmed = TrimCopy(RawValue);

     if (Trimmed.empty())
     {
          return Values;
     }

     try
     {
          if (!Trimmed.empty() && Trimmed.front() == '[')
          {
               const nlohmann::json Parsed = nlohmann::json::parse(Trimmed);

               if (Parsed.is_array())
               {
                    for (const auto& Entry : Parsed)
                    {
                         if (!Entry.is_string())
                         {
                              continue;
                         }

                         const std::string Candidate = TrimCopy(Entry.get<std::string>());

                         if (!Candidate.empty())
                         {
                              Values.push_back(Candidate);
                         }

                         if (Values.size() >= MaxValues)
                         {
                              break;
                         }
                    }
               }

               return Values;
          }
     }
     catch (...)
     {
     }

     std::string Token;
     std::istringstream Input(Trimmed);

     while (std::getline(Input, Token, ','))
     {
          size_t Start = 0;

          while (Start < Token.size())
          {
               size_t End = Token.find_first_of(";\n|", Start);
               std::string Candidate = TrimCopy(Token.substr(Start, End == std::string::npos ? std::string::npos : End - Start));

               if (!Candidate.empty())
               {
                    Values.push_back(Candidate);
               }

               if (Values.size() >= MaxValues || End == std::string::npos)
               {
                    break;
               }

               Start = End + 1;
          }

          if (Values.size() >= MaxValues)
          {
               break;
          }
     }

     if (Values.empty())
     {
          Values.push_back(Trimmed);
     }

     return Values;
}

std::vector<std::string> BuildSAMSentenceSamples(const std::string& RawValue,
                                                 size_t MaxSamples = 10)
{
     std::vector<std::string> Samples;
     std::string Current;

     for (unsigned char C : RawValue)
     {
          if (C == '\r')
          {
               continue;
          }

          if (C == '\n' || C == '.' || C == '!' || C == '?' || C == ';')
          {
               const std::string Candidate = TrimCopy(Current);

               if (!Candidate.empty())
               {
                    Samples.push_back(Candidate);
               }

               Current.clear();

               if (Samples.size() >= MaxSamples)
               {
                    break;
               }

               continue;
          }

          Current.push_back(static_cast<char>(C));
     }

     if (Samples.size() < MaxSamples)
     {
          const std::string Candidate = TrimCopy(Current);

          if (!Candidate.empty())
          {
               Samples.push_back(Candidate);
          }
     }

     return Samples;
}

/* Count useful evidence tokens used to score document-derived phrases. */

size_t CountStrongSAMTokens(const std::vector<std::string>& Tokens)
{
     size_t Count = 0;

     for (const auto& Token : Tokens)
     {
          if (!Token.empty() && !IsSamStopword(Token) && !IsWeakSamToken(Token) && !IsNumericLikeSAMToken(Token))
          {
               ++Count;
          }
     }

     return Count;
}

bool IsUsefulSAMDocumentPhrase(const std::string& Value)
{
     const std::string Normalized = NormalizeTerm(Value);

     if (Normalized.empty() || Normalized.size() < 2 || Normalized.size() > 96)
     {
          return false;
     }

     const std::vector<std::string> Tokens = TokenizeNormalized(Normalized);

     if (Tokens.empty() || Tokens.size() > 7)
     {
          return false;
     }

     const size_t StrongCount = CountStrongSAMTokens(Tokens);

     if (StrongCount == 0)
     {
          return false;
     }

     if (Tokens.size() == 1)
     {
          return StrongCount == 1 && Tokens.front().size() >= 3;
     }

     if (StrongCount == 1 && Tokens.size() >= 4)
     {
          return false;
     }

     return !IsIdentifierLikeSAMValue(Value);
}

std::vector<std::string> ExtractStrongSAMTokens(const std::string& Value)
{
     std::vector<std::string> StrongTokens;

     for (const auto& Token : NormalizeSAMTokens(Value, true))
     {
          const std::string Singular = SingularizeToken(Token);

          if (Singular.empty() || IsWeakSamToken(Singular) || IsNumericLikeSAMToken(Singular))
          {
               continue;
          }

          if (std::find(StrongTokens.begin(), StrongTokens.end(), Singular) == StrongTokens.end())
          {
               StrongTokens.push_back(Singular);
          }
     }

     return StrongTokens;
}

bool ShouldSkipSAMPairPhrase(const std::string& SubjectSeed,
                             const std::string& CandidateSeed)
{
     const std::string NormalizedSubject = NormalizeTerm(SubjectSeed);
     const std::string NormalizedCandidate = NormalizeTerm(CandidateSeed);

     if (NormalizedSubject.empty() || NormalizedCandidate.empty())
     {
          return true;
     }

     if (NormalizedSubject == NormalizedCandidate)
     {
          return true;
     }

     const std::vector<std::string> SubjectStrongTokens = ExtractStrongSAMTokens(NormalizedSubject);
     const std::vector<std::string> CandidateStrongTokens = ExtractStrongSAMTokens(NormalizedCandidate);

     if (CandidateStrongTokens.empty())
     {
          return true;
     }

     size_t NewStrongTokens = 0;

     for (const auto& Token : CandidateStrongTokens)
     {
          if (std::find(SubjectStrongTokens.begin(), SubjectStrongTokens.end(), Token) == SubjectStrongTokens.end())
          {
               ++NewStrongTokens;
          }
     }

     if (NewStrongTokens == 0)
     {
          return true;
     }

     const std::vector<std::string> CandidateTokens = TokenizeNormalized(NormalizedCandidate);

     if (!CandidateTokens.empty())
     {
          const std::string& First = CandidateTokens.front();
          const std::string& Last = CandidateTokens.back();

          if ((IsWeakSamToken(First) || IsWeakSamToken(Last)) && NewStrongTokens < 2)
          {
               return true;
          }
     }

     return false;
}

/* Join a contiguous token range into a normalized phrase. */

std::string JoinSAMTokenRange(const std::vector<std::string>& Tokens,
                              size_t Start,
                              size_t End)
{
     std::string Result;

     for (size_t Index = Start; Index < End; ++Index)
     {
          if (!Result.empty())
          {
               Result.push_back(' ');
          }

          Result += Tokens[Index];
     }

     return Result;
}

std::vector<std::string> BuildSAMPhraseWindows(const std::string& Value,
                                               size_t MinTokens,
                                               size_t MaxTokens,
                                               size_t MaxPhrases)
{
     std::vector<std::string> Result;
     std::unordered_set<std::string> Seen;
     const std::vector<std::string> Tokens = TokenizeNormalized(NormalizeTerm(Value));

     if (Tokens.empty())
     {
          return Result;
     }

     for (size_t Window = MinTokens; Window <= MaxTokens; ++Window)
     {
          if (Window == 0 || Window > Tokens.size())
          {
               continue;
          }

          for (size_t Start = 0; Start + Window <= Tokens.size(); ++Start)
          {
               const std::string Candidate = JoinSAMTokenRange(Tokens, Start, Start + Window);

               if (!IsUsefulSAMDocumentPhrase(Candidate) || !Seen.insert(Candidate).second)
               {
                    continue;
               }

               const std::vector<std::string> CandidateTokens = TokenizeNormalized(Candidate);
               const size_t StrongCount = CountStrongSAMTokens(CandidateTokens);

               if (Window >= 3 && StrongCount < 2)
               {
                    continue;
               }

               Result.push_back(Candidate);

               if (Result.size() >= MaxPhrases)
               {
                    return Result;
               }
          }
     }

     return Result;
}

std::vector<std::string> BuildSAMReducedTitleVariants(const std::string& Title)
{
     std::vector<std::string> Variants;
     const std::vector<std::string> Tokens = TokenizeNormalized(NormalizeTerm(Title));

     if (Tokens.size() < 2)
     {
          return Variants;
     }

     std::vector<std::string> Reduced = Tokens;

     while (!Reduced.empty() &&
            (IsSamStopword(Reduced.back()) || IsWeakSamToken(Reduced.back())))
     {
          Reduced.pop_back();
     }

     if (Reduced.size() >= 2 && Reduced.size() < Tokens.size())
     {
          Variants.push_back(JoinTokens(Reduced));
     }

     Reduced = Tokens;

     while (!Reduced.empty() &&
            (IsSamStopword(Reduced.front()) || IsWeakSamToken(Reduced.front())))
     {
          Reduced.erase(Reduced.begin());
     }

     if (Reduced.size() >= 2 && Reduced.size() < Tokens.size())
     {
          Variants.push_back(JoinTokens(Reduced));
     }

     if (Tokens.size() >= 3)
     {
          Variants.push_back(JoinSAMTokenRange(Tokens, 0, Tokens.size() - 1));
          Variants.push_back(JoinSAMTokenRange(Tokens, 1, Tokens.size()));
     }

     return UniqueNormalizedPhrases(Variants, 8);
}

std::vector<std::string> BuildSAMSeedQueries(const std::vector<std::string>& Subjects,
                                             const std::vector<std::string>& Descriptors)
{
     std::vector<std::string> Queries;
     std::unordered_set<std::string> Seen;

     auto Append = [&](const std::string& Value)
     {
          const std::string Candidate = NormalizeTerm(Value);

          if (!IsUsefulSAMDocumentPhrase(Candidate) || !Seen.insert(Candidate).second)
          {
               return;
          }

          Queries.push_back(Candidate);
     };

     for (const auto& Subject : Subjects)
     {
          Append(Subject);
          Append(Subject + " guide");
          Append(Subject + " overview");
     }

     for (const auto& Subject : Subjects)
     {
          for (const auto& Descriptor : Descriptors)
          {
               Append(Subject + " " + Descriptor);
               Append(Descriptor + " " + Subject);

               if (Queries.size() >= 24)
               {
                    return Queries;
               }
          }
     }

     return Queries;
}

std::string SpellSAMEnglishUnder100(int Value)
{
     static const std::array<const char*, 20> Units = {
          "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
          "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen",
          "seventeen", "eighteen", "nineteen"
     };
     static const std::array<const char*, 10> Tens = {
          "", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"
     };

     if (Value < 0 || Value >= 100)
     {
          return "";
     }

     if (Value < 20)
     {
          return Units[static_cast<size_t>(Value)];
     }

     const int TensValue = Value / 10;
     const int UnitValue = Value % 10;

     if (UnitValue == 0)
     {
          return Tens[static_cast<size_t>(TensValue)];
     }

     return std::string(Tens[static_cast<size_t>(TensValue)]) + " " +
            Units[static_cast<size_t>(UnitValue)];
}

std::string SpellSAMSpanishUnder100(int Value)
{
     static const std::array<const char*, 30> Units = {
          "cero", "uno", "dos", "tres", "cuatro", "cinco", "seis", "siete", "ocho", "nueve",
          "diez", "once", "doce", "trece", "catorce", "quince", "dieciseis", "diecisiete",
          "dieciocho", "diecinueve", "veinte", "veintiuno", "veintidos", "veintitres",
          "veinticuatro", "veinticinco", "veintiseis", "veintisiete", "veintiocho", "veintinueve"
     };
     static const std::array<const char*, 10> Tens = {
          "", "", "", "treinta", "cuarenta", "cincuenta", "sesenta", "setenta", "ochenta", "noventa"
     };

     if (Value < 0 || Value >= 100)
     {
          return "";
     }

     if (Value < 30)
     {
          return Units[static_cast<size_t>(Value)];
     }

     const int TensValue = Value / 10;
     const int UnitValue = Value % 10;

     if (UnitValue == 0)
     {
          return Tens[static_cast<size_t>(TensValue)];
     }

     return std::string(Tens[static_cast<size_t>(TensValue)]) + " y " +
            Units[static_cast<size_t>(UnitValue)];
}

/* Spell a compact integer value as English words for numeric matching. */

std::string SpellSAMEnglishNumber(int Value)
{
     if (Value < 0 || Value > 9999)
     {
          return "";
     }

     if (Value < 100)
     {
          return SpellSAMEnglishUnder100(Value);
     }

     if (Value < 1000)
     {
          const int Hundreds = Value / 100;
          const int Remainder = Value % 100;
          std::string Result = SpellSAMEnglishUnder100(Hundreds) + " hundred";

          if (Remainder > 0)
          {
               Result += " " + SpellSAMEnglishUnder100(Remainder);
          }

          return Result;
     }

     const int Thousands = Value / 1000;
     const int Remainder = Value % 1000;
     std::string Result = SpellSAMEnglishUnder100(Thousands) + " thousand";

     if (Remainder >= 100)
     {
          Result += " " + SpellSAMEnglishNumber(Remainder);
     }
     else if (Remainder > 0)
     {
          Result += " " + SpellSAMEnglishUnder100(Remainder);
     }

     return Result;
}

std::string SpellSAMSpanishNumber(int Value)
{
     if (Value < 0 || Value > 9999)
     {
          return "";
     }

     if (Value < 100)
     {
          return SpellSAMSpanishUnder100(Value);
     }

     if (Value == 100)
     {
          return "cien";
     }

     if (Value < 1000)
     {
          static const std::array<const char*, 10> Hundreds = {
               "", "ciento", "doscientos", "trescientos", "cuatrocientos",
               "quinientos", "seiscientos", "setecientos", "ochocientos", "novecientos"
          };

          const int HundredsValue = Value / 100;
          const int Remainder = Value % 100;
          std::string Result = Hundreds[static_cast<size_t>(HundredsValue)];

          if (Remainder > 0)
          {
               Result += " " + SpellSAMSpanishUnder100(Remainder);
          }

          return Result;
     }

     const int Thousands = Value / 1000;
     const int Remainder = Value % 1000;
     std::string Result;

     if (Thousands == 1)
     {
          Result = "mil";
     }
     else
     {
          Result = SpellSAMSpanishUnder100(Thousands) + " mil";
     }

     if (Remainder > 0)
     {
          Result += " " + SpellSAMSpanishNumber(Remainder);
     }

     return Result;
}

std::string SpellSAMNumericToken(const std::string& Token, const std::string& Lang)
{
     if (!IsNumericLikeSAMToken(Token) || Token.empty() || Token.size() > 4)
     {
          return "";
     }

     if (Token.size() > 1 && Token.front() == '0')
     {
          return "";
     }

     int Value = 0;
     const char* Begin = Token.data();
     const char* End = Begin + Token.size();
     const auto Parsed = std::from_chars(Begin, End, Value);
     if (Parsed.ec != std::errc{} || Parsed.ptr != End)
     {
          return "";
     }

     if (Lang.rfind("es", 0) == 0)
     {
          return SpellSAMSpanishNumber(Value);
     }

     if (Lang.empty() || Lang == "und" || Lang.rfind("en", 0) == 0)
     {
          return SpellSAMEnglishNumber(Value);
     }

     return "";
}

std::vector<std::string> BuildSAMNumericWordVariants(const std::string& Value,
                                                     const std::string& Lang,
                                                     size_t MaxVariants = 3)
{
     std::vector<std::string> Variants;
     const std::string Normalized = NormalizeTerm(Value);
     const std::vector<std::string> Tokens = TokenizeNormalized(Normalized);

     if (Tokens.empty() || Tokens.size() > 6)
     {
          return Variants;
     }

     std::vector<std::string> Rewritten = Tokens;
     size_t NumericTokenCount = 0;
     bool Changed = false;

     for (size_t Index = 0; Index < Tokens.size(); ++Index)
     {
          const std::string Spelled = SpellSAMNumericToken(Tokens[Index], Lang);

          if (Spelled.empty())
          {
               continue;
          }

          Rewritten[Index] = Spelled;
          ++NumericTokenCount;
          Changed = true;
     }

     if (!Changed || NumericTokenCount == 0 || NumericTokenCount > 2)
     {
          return Variants;
     }

     const std::string Candidate = JoinTokens(Rewritten);

     if (Candidate != Normalized && IsUsefulSAMDocumentPhrase(Candidate))
     {
          Variants.push_back(Candidate);
     }

     if (Variants.size() > MaxVariants)
     {
          Variants.resize(MaxVariants);
     }

     return Variants;
}

struct SAMTermCollector
{
     std::unordered_map<std::string, SAM::TermEntry> Entries;
     std::unordered_map<std::string, size_t> Support;

     void Add(const std::string& Text,
              const std::string& Kind,
              const std::string& Source,
              double Score,
              double Signal)
     {
          const std::string Normalized = NormalizeTerm(Text);

          if (!IsUsefulSAMDocumentPhrase(Normalized))
          {
               return;
          }

          SAM::TermEntry& Entry = Entries[Normalized];
          size_t& EntrySupport = Support[Normalized];
          ++EntrySupport;

          if (Entry.Text.empty() ||
              Score > Entry.Score ||
              (Score == Entry.Score && Signal > Entry.Signal))
          {
               Entry.Text = Normalized;
               Entry.Kind = Kind;
               Entry.Source = Source;
          }

          Entry.Score = std::max(Entry.Score, ClampSAMScore(Score));
          Entry.Signal = std::max(Entry.Signal,
                                  ClampSAMScore(Signal + std::min(0.16, 0.03 * static_cast<double>(EntrySupport - 1))));
     }

     std::vector<SAM::TermEntry> Finalize(size_t Limit = 96) const
     {
          std::vector<SAM::TermEntry> Terms;
          Terms.reserve(Entries.size());

          for (const auto& Pair : Entries)
          {
               Terms.push_back(Pair.second);
          }

          std::sort(Terms.begin(), Terms.end(),
                    [](const SAM::TermEntry& Left, const SAM::TermEntry& Right)
                    {
                         if (Left.Score != Right.Score)
                         {
                              return Left.Score > Right.Score;
                         }

                         if (Left.Signal != Right.Signal)
                         {
                              return Left.Signal > Right.Signal;
                         }

                         if (Left.Text.size() != Right.Text.size())
                         {
                              return Left.Text.size() < Right.Text.size();
                         }

                         return Left.Text < Right.Text;
                    });

          if (Terms.size() > Limit)
          {
               Terms.resize(Limit);
          }

          return Terms;
     }
};

std::unordered_set<std::string> BuildSAMDocumentEvidenceTokens(const Document& Doc,
                                                               size_t MaxTokens = 512)
{
     std::unordered_set<std::string> Tokens;

     auto AppendTokens = [&](const std::string& Value)
     {
          for (const auto& Token : NormalizeSAMTokens(Value, true))
          {
               if (Token.empty())
               {
                    continue;
               }

               Tokens.insert(Token);

               if (Tokens.size() >= MaxTokens)
               {
                    return;
               }
          }
     };

     AppendTokens(Doc.Title);
     AppendTokens(Doc.Content);

     for (const auto& Field : CollectDocumentTextFields(Doc))
     {
          if (Tokens.size() >= MaxTokens)
          {
               break;
          }

          AppendTokens(Field.second);
     }

     return Tokens;
}

size_t CountSAMTokenOverlap(const std::vector<std::string>& CandidateTokens,
                            const std::unordered_set<std::string>& EvidenceTokens)
{
     size_t Overlap = 0;

     for (const auto& Token : CandidateTokens)
     {
          if (EvidenceTokens.find(Token) != EvidenceTokens.end())
          {
               ++Overlap;
          }
     }

     return Overlap;
}

bool HasConsistentSAMCandidateLanguage(const std::string& Collection,
                                       const std::string& Candidate,
                                       const std::string& DocumentLang)
{
     if (Candidate.empty() || DocumentLang.empty() || DocumentLang == "und")
     {
          return true;
     }

     Document CandidateDoc;
     CandidateDoc.ID = Candidate;
     CandidateDoc.Title = Candidate;
     const std::string CandidateLang = sam::lang::DetectDocumentLanguage(Collection, CandidateDoc);

     return CandidateLang == "und" || CandidateLang == DocumentLang;
}

/* Validate an LLM candidate against source-document evidence. */

bool ValidateSAMLLMCandidate(const std::string& Collection,
                             const Document& Doc,
                             const std::string& Candidate,
                             const std::string& Subject,
                             const std::unordered_set<std::string>& EvidenceTokens,
                             const std::string& DocumentLang)
{
     if (!IsUsefulSAMDocumentPhrase(Candidate) || IsIdentifierLikeSAMValue(Candidate))
     {
          return false;
     }

     if (!HasConsistentSAMCandidateLanguage(Collection, Candidate, DocumentLang))
     {
          return false;
     }

     const std::vector<std::string> CandidateTokens = NormalizeSAMTokens(Candidate, true);

     if (CandidateTokens.empty())
     {
          return false;
     }

     const size_t Overlap = CountSAMTokenOverlap(CandidateTokens, EvidenceTokens);
     const bool MentionsSubject = !Subject.empty() &&
          (Candidate == Subject ||
           Candidate.find(Subject) != std::string::npos ||
           Subject.find(Candidate) != std::string::npos);

     if (Overlap == 0 && !MentionsSubject)
     {
          return false;
     }

     if (CandidateTokens.size() >= 3 && Overlap == 0)
     {
          return false;
     }

     if (CandidateTokens.size() >= 2 && Overlap == 1 && !MentionsSubject)
     {
          return false;
     }

     (void)Doc;
     return true;
}

std::unordered_map<std::string, double> BuildSAMLLMFeedbackBoosts(rocksdb::DB* Database,
                                                                  const std::string& Collection,
                                                                  const Document& Doc,
                                                                  const std::string& Subject,
                                                                  const std::unordered_set<std::string>& EvidenceTokens,
                                                                  const std::string& DocumentLang,
                                                                  size_t MaxIdeas = 64)
{
     std::unordered_map<std::string, double> Boosts;

     if (!Database || Collection.empty() || Doc.ID.empty() || MaxIdeas == 0)
     {
          return Boosts;
     }

     const std::string Prefix = BuildSearchIdeaPrefix(Collection);
     std::unique_ptr<rocksdb::Iterator> Iterator(Database->NewIterator(rocksdb::ReadOptions()));
     const uint64_t NowMS = GetSAMCurrentTimeMS();
     size_t SeenIdeas = 0;

     auto AddBoost = [&](const std::string& Value, double Boost)
     {
          const std::string Candidate = NormalizeTerm(Value);

          if (!ValidateSAMLLMCandidate(Collection,
                                       Doc,
                                       Candidate,
                                       Subject,
                                       EvidenceTokens,
                                       DocumentLang))
          {
               return;
          }

          Boosts[Candidate] = std::max(Boosts[Candidate], ClampSAMScore(Boost));
     };

     for (Iterator->Seek(Prefix);
          Iterator->Valid() && Iterator->key().starts_with(Prefix) && SeenIdeas < MaxIdeas;
          Iterator->Next(), ++SeenIdeas)
     {
          SAM::SearchIdeaEntry Entry;

          if (!ParseSearchIdeaEntry(Iterator->value().ToString(), Entry) || Entry.Uses == 0)
          {
               continue;
          }

          bool TargetsDocument = false;

          for (const auto& DocumentRef : Entry.Documents)
          {
               if (DocumentRef.DocumentID == Doc.ID)
               {
                    TargetsDocument = true;
                    break;
               }
          }

          if (!TargetsDocument)
          {
               continue;
          }

          const double Popularity = ClampSAMScore(std::log1p(static_cast<double>(Entry.Uses)) / std::log(12.0));
          const double Freshness = GetSAMIdeaFreshness(Entry.LastSeenMS, NowMS);
          const double Resolution = Entry.ResolvedAtMS > 0
               ? ClampSAMScore(std::log1p(static_cast<double>(std::max<uint64_t>(Entry.ResolvedUses, 1))) / std::log(12.0))
               : 0.0;
          const double BaseBoost = ClampSAMScore((Popularity * 0.50) + (Freshness * 0.25) + (Resolution * 0.25));

          AddBoost(Entry.Query, BaseBoost * 0.92);

          for (const auto& Candidate : Entry.ResolvedCandidates)
          {
               AddBoost(Candidate.Text, BaseBoost * (0.84 + (ClampSAMScore(Candidate.Weight) * 0.16)));
          }

          for (const auto& RankedTerm : Entry.ResolvedRankedTerms)
          {
               AddBoost(RankedTerm.Text, BaseBoost * (0.88 + (ClampSAMScore(RankedTerm.Weight) * 0.20)));
          }
     }

     return Boosts;
}

std::vector<SAM::TermEntry> SAM::GenerateLLMTerms(const std::string& Collection,
                                                  const Document& Doc,
                                                  std::string* ErrorMessage) const
{
     std::vector<TermEntry> Terms;

     if (!(Instance && Instance->LLM))
     {
          return Terms;
     }

     try
     {
          const std::string DocumentLang = sam::lang::DetectDocumentLanguage(Collection, Doc);
          const std::unordered_set<std::string> EvidenceTokens = BuildSAMDocumentEvidenceTokens(Doc);
          std::unordered_map<std::string, double> FeedbackBoosts;
          const std::vector<llm::AnchorSuggestion> Anchors =
               Instance->LLM->BuildDocumentAnchors(Collection, Doc, DocumentLang, 10);
          const std::vector<llm::ContextSuggestion> Suggestions =
               Instance->LLM->BuildDocumentContext(Collection, Doc, 8);
          const std::string Subject = NormalizeTerm(Doc.Title.empty() ? Doc.ID : Doc.Title);

          {
               std::lock_guard<std::mutex> Lock(DBMutex);

               if (Database)
               {
                    FeedbackBoosts = BuildSAMLLMFeedbackBoosts(Database.get(),
                                                               Collection,
                                                               Doc,
                                                               Subject,
                                                               EvidenceTokens,
                                                               DocumentLang);
               }
          }

          for (const auto& Anchor : Anchors)
          {
               const std::string Candidate = NormalizeTerm(Anchor.Text);

               if (!ValidateSAMLLMCandidate(Collection,
                                            Doc,
                                            Candidate,
                                            Subject,
                                            EvidenceTokens,
                                            DocumentLang))
               {
                    continue;
               }

               std::string Kind = "descriptor";
               std::string Source = "llm_anchor";

               if (Anchor.Kind == "query")
               {
                    Kind = "query";
                    Source = "llm_query";
               }
               else if (Anchor.Kind == "alias")
               {
                    Kind = "alias";
                    Source = "llm_alias";
               }
               else if (Anchor.Kind == "descriptor")
               {
                    Kind = "descriptor";
                    Source = "llm_descriptor";
               }
               else if (Anchor.Kind == "anchor" || Anchor.Kind == "subject")
               {
                    Kind = (!Subject.empty() &&
                            (Candidate == Subject ||
                             Candidate.find(Subject) != std::string::npos ||
                             Subject.find(Candidate) != std::string::npos))
                         ? "alias"
                         : "descriptor";
                    Source = "llm_anchor";
               }

               const double FeedbackBoost = FeedbackBoosts.count(Candidate) > 0
                    ? FeedbackBoosts[Candidate]
                    : 0.0;
               const double Score = ClampSAMScore(0.66 +
                                                  (ClampSAMScore(Anchor.Confidence) * 0.20) +
                                                  std::min(0.16, FeedbackBoost * 0.16));
               const double Signal = ClampSAMScore(0.70 +
                                                   (ClampSAMScore(Anchor.Confidence) * 0.18) +
                                                   std::min(0.18, FeedbackBoost * 0.18));

               Terms.push_back(TermEntry{Candidate, Kind, Source, Score, Signal});

               if (!Subject.empty() && Candidate != Subject && Kind != "query")
               {
                    Terms.push_back(TermEntry{
                        Subject + " " + Candidate,
                        "query",
                        "llm_anchor_pair",
                         std::max(0.68, Score - 0.04),
                         std::max(0.72, Signal - 0.04)
                    });
               }
          }

          for (const auto& Suggestion : Suggestions)
          {
               const std::string Candidate = NormalizeTerm(Suggestion.Text);

               if (!ValidateSAMLLMCandidate(Collection,
                                            Doc,
                                            Candidate,
                                            Subject,
                                            EvidenceTokens,
                                            DocumentLang))
               {
                    continue;
               }

               const bool IsSubjectSuggestion = !Subject.empty() &&
                    (Candidate == Subject ||
                     Candidate.find(Subject) != std::string::npos ||
                     Subject.find(Candidate) != std::string::npos);
               const double FeedbackBoost = FeedbackBoosts.count(Candidate) > 0
                    ? FeedbackBoosts[Candidate]
                    : 0.0;
               const double Score = ClampSAMScore((Suggestion.Kind == "llm" ? 0.78 : 0.70) +
                                                  std::min(0.16, FeedbackBoost * 0.16));
               const double Signal = ClampSAMScore((Suggestion.Kind == "llm" ? 0.82 : 0.72) +
                                                   std::min(0.18, FeedbackBoost * 0.18));

               Terms.push_back(TermEntry{
                    Candidate,
                    IsSubjectSuggestion ? "alias" : (Suggestion.Kind == "llm" ? "query" : "descriptor"),
                    Suggestion.Kind == "llm" ? "llm_context" : "context_field",
                    Score,
                    Signal
               });

               if (!Subject.empty() && Candidate != Subject)
               {
                    Terms.push_back(TermEntry{
                        Subject + " " + Candidate,
                        "query",
                        Suggestion.Kind == "llm" ? "llm_pair" : "context_pair",
                        std::max(0.68, Score - 0.04),
                        std::max(0.70, Signal - 0.04)
                    });
               }
          }
     }
     catch (const std::exception& E)
     {
          if (ErrorMessage && ErrorMessage->empty())
          {
               *ErrorMessage = E.what();
          }
     }

     return Terms;
}

std::vector<SAM::TermEntry> SAM::GenerateLLMTermsFromProfile(const std::string& Collection,
                                                             const Document& Doc,
                                                             const std::vector<std::string>& ProfileTerms,
                                                             std::string* ErrorMessage) const
{
     (void)Collection;
     std::vector<TermEntry> Terms;

     if (ProfileTerms.empty())
     {
          return Terms;
     }

     try
     {
          const std::string Subject = NormalizeTerm(Doc.Title.empty() ? Doc.ID : Doc.Title);
          std::unordered_set<std::string> DocumentTokens;

          for (const auto& Field : CollectDocumentTextFields(Doc))
          {
               const std::vector<std::string> Tokens = NormalizeSAMTokens(Field.second, true);

               for (const auto& Token : Tokens)
               {
                    if (!Token.empty())
                    {
                         DocumentTokens.insert(Token);
                    }
               }
          }

          for (const auto& ProfileTerm : ProfileTerms)
          {
               const std::string Candidate = NormalizeTerm(ProfileTerm);

               if (!IsUsefulSAMDocumentPhrase(Candidate))
               {
                    continue;
               }

               const std::vector<std::string> CandidateTokens = NormalizeSAMTokens(Candidate, true);
               size_t Overlap = 0;

               for (const auto& Token : CandidateTokens)
               {
                    if (DocumentTokens.find(Token) != DocumentTokens.end())
                    {
                         ++Overlap;
                    }
               }

               if (Overlap == 0 && !Subject.empty() && Candidate.find(Subject) == std::string::npos)
               {
                    continue;
               }

               const double OverlapBoost = std::min(0.18, 0.05 * static_cast<double>(Overlap));

               Terms.push_back(TermEntry{
                    Candidate,
                    Overlap >= 2 ? "alias" : "descriptor",
                    "profile_context",
                    0.72 + OverlapBoost,
                    0.74 + OverlapBoost
               });

               if (!Subject.empty() && Candidate != Subject)
               {
                    Terms.push_back(TermEntry{
                         Subject + " " + Candidate,
                         "query",
                         "profile_pair",
                         0.70 + OverlapBoost,
                         0.76 + OverlapBoost
                    });
               }
          }
     }
     catch (const std::exception& E)
     {
          if (ErrorMessage && ErrorMessage->empty())
          {
               *ErrorMessage = E.what();
          }
     }

     return Terms;
}

std::vector<SAM::TermEntry> SAM::ExpandDocumentTerms(const std::string& Collection,
                                                     const Document& Doc,
                                                     std::string* ErrorMessage) const
{
     if (Collection.empty() || Doc.ID.empty())
     {
          if (ErrorMessage)
          {
               *ErrorMessage = "Collection and document ID are required.";
          }

          return {};
     }

     SAMTermCollector Collector;
     std::vector<std::string> SubjectSeeds;
     std::vector<std::string> DescriptorSeeds;
     std::vector<std::string> QuerySeeds;
     std::unordered_set<std::string> SubjectSeen;
     std::unordered_set<std::string> DescriptorSeen;
     std::unordered_set<std::string> QuerySeen;
     const std::string DocumentLang = sam::lang::DetectDocumentLanguage(Collection, Doc);

     auto RememberSeed = [](std::vector<std::string>& Output,
                            std::unordered_set<std::string>& Seen,
                            const std::string& Value,
                            size_t Limit)
     {
          const std::string Candidate = NormalizeTerm(Value);

          if (Candidate.empty() || !Seen.insert(Candidate).second)
          {
               return;
          }

          Output.push_back(Candidate);

          if (Output.size() > Limit)
          {
               Output.resize(Limit);
          }
     };

     auto AddTerm = [&](const std::string& Text,
                        const std::string& Kind,
                        const std::string& Source,
                        double Score,
                        double Signal)
     {
          Collector.Add(Text, Kind, Source, Score, Signal);

          for (const auto& NumericVariant : BuildSAMNumericWordVariants(Text, DocumentLang))
          {
               Collector.Add(NumericVariant,
                             Kind == "subject" ? "alias" : Kind,
                             Source,
                             std::max(0.54, Score - 0.06),
                             std::max(0.58, Signal - 0.04));
          }

          if (Kind == "subject" || Kind == "alias" || Kind == "synonym")
          {
               RememberSeed(SubjectSeeds, SubjectSeen, Text, 16);
          }
          else if (Kind == "descriptor")
          {
               RememberSeed(DescriptorSeeds, DescriptorSeen, Text, 20);
          }
          else if (Kind == "query")
          {
               RememberSeed(QuerySeeds, QuerySeen, Text, 20);
          }
     };

     const std::string RawTitle = TrimCopy(Doc.Title.empty() ? Doc.ID : Doc.Title);
     const std::string Subject = NormalizeTerm(RawTitle);

     if (!Subject.empty())
     {
          AddTerm(Subject, "subject", "title", 0.99, 0.99);

          for (const auto& Variant : BuildSAMReducedTitleVariants(RawTitle))
          {
               AddTerm(Variant, "alias", "title_reduced", 0.93, 0.93);
          }

          for (const auto& Window : BuildSAMPhraseWindows(RawTitle, 1, 4, 12))
          {
               AddTerm(Window,
                       Window == Subject ? "subject" : "alias",
                       Window == Subject ? "title" : "title_window",
                       Window == Subject ? 0.97 : 0.88,
                       Window == Subject ? 0.97 : 0.88);
          }
     }

     std::string CombinedNarrative;

     for (const auto& Field : CollectDocumentTextFields(Doc))
     {
          const std::string& LowerKey = Field.first;
          const std::string& RawValue = Field.second;

          if (RawValue.empty())
          {
               continue;
          }

          const bool TitleLike = IsTitleLikeSAMField(LowerKey);
          const bool SummaryLike = IsSummaryLikeSAMField(LowerKey);
          const bool BodyLike = IsBodyLikeSAMField(LowerKey);
          const bool AliasLike = IsAliasLikeSAMField(LowerKey);
          const bool TaxonomyLike = IsTaxonomyLikeSAMField(LowerKey);
          const bool QueryLike = IsQueryLikeSAMField(LowerKey);

          if ((SummaryLike || BodyLike) && CombinedNarrative.size() < 4000)
          {
               if (!CombinedNarrative.empty())
               {
                    CombinedNarrative.push_back(' ');
               }

               CombinedNarrative += TruncateSAMDocumentText(RawValue, 1800);
          }

          const std::vector<std::string> Values =
               ExtractArrayishSAMValues(RawValue,
                                        TaxonomyLike || AliasLike || QueryLike ? 12 : 4);

          for (const auto& Value : Values)
          {
               if (Value.empty() || IsIdentifierLikeSAMValue(Value))
               {
                    continue;
               }

               if (TitleLike)
               {
                    AddTerm(Value, "alias", "field_title", 0.91, 0.92);
               }
               else if (AliasLike)
               {
                    AddTerm(Value, "alias", "alias_field", 0.89, 0.90);
               }
               else if (TaxonomyLike)
               {
                    AddTerm(Value, "descriptor", "taxonomy_field", 0.84, 0.86);
               }
               else if (QueryLike)
               {
                    AddTerm(Value, "query", "query_field", 0.82, 0.84);
               }
               else if (SummaryLike)
               {
                    AddTerm(Value, "descriptor", "summary_field", 0.76, 0.78);
               }

               const size_t MinWindow = (TaxonomyLike || AliasLike || QueryLike) ? 1 : 2;
               const size_t MaxWindow = (TaxonomyLike || AliasLike || QueryLike) ? 3 : 4;
               const size_t WindowLimit = (TaxonomyLike || AliasLike || QueryLike) ? 8 : 5;

               for (const auto& Window : BuildSAMPhraseWindows(Value, MinWindow, MaxWindow, WindowLimit))
               {
                    const std::string Kind =
                         AliasLike ? "alias" :
                         QueryLike ? "query" :
                         (TaxonomyLike || SummaryLike ? "descriptor" : "alias");
                    const std::string Source =
                         AliasLike ? "alias_window" :
                         QueryLike ? "query_window" :
                         TaxonomyLike ? "taxonomy_window" :
                         SummaryLike ? "summary_window" : "field_window";
                    const double BaseScore =
                         AliasLike ? 0.83 :
                         QueryLike ? 0.80 :
                         TaxonomyLike ? 0.78 :
                         SummaryLike ? 0.74 : 0.72;

                    AddTerm(Window, Kind, Source, BaseScore, BaseScore);
               }

               if (!Subject.empty() && NormalizeTerm(Value) != Subject)
               {
                    const std::string LoweredValue = NormalizeTerm(Value);

                    if (ShouldSkipSAMPairPhrase(Subject, LoweredValue))
                    {
                         continue;
                    }

                    const std::string Kind =
                         QueryLike ? "query" :
                         TaxonomyLike ? "query" :
                         AliasLike ? "alias" : "descriptor";
                    const std::string Source =
                         QueryLike ? "query_pair" :
                         TaxonomyLike ? "subject_descriptor" :
                         AliasLike ? "subject_alias" : "field_pair";
                    AddTerm(Subject + " " + LoweredValue, Kind, Source, 0.86, 0.87);

                    if (TaxonomyLike || AliasLike)
                    {
                         AddTerm(LoweredValue + " " + Subject, Kind, Source, 0.82, 0.84);
                    }
               }
          }

          if (SummaryLike || BodyLike)
          {
               const std::vector<std::string> Samples =
                    BuildSAMSentenceSamples(RawValue, SummaryLike ? 6 : 10);

               for (const auto& Sample : Samples)
               {
                    for (const auto& Window : BuildSAMPhraseWindows(Sample, 2, 5, SummaryLike ? 6 : 8))
                    {
                         const std::vector<std::string> WindowTokens = NormalizeSAMTokens(Window, true);
                         const size_t StrongCount = CountStrongSAMTokens(WindowTokens);

                         if (StrongCount < 2)
                         {
                              continue;
                         }

                         AddTerm(Window,
                                 StrongCount >= 3 ? "query" : "descriptor",
                                 SummaryLike ? "summary_window" : "body_window",
                                 SummaryLike ? 0.76 : 0.70,
                                 SummaryLike ? 0.78 : 0.72);
                    }
               }
          }
     }

     if (!CombinedNarrative.empty())
     {
          for (const auto& Window : BuildSAMPhraseWindows(CombinedNarrative, 2, 5, 16))
          {
               const std::vector<std::string> Tokens = NormalizeSAMTokens(Window, true);

               if (CountStrongSAMTokens(Tokens) < 2)
               {
                    continue;
               }

               AddTerm(Window, "query", "body_query", 0.68, 0.72);
          }
     }

     const std::vector<std::string> SeedQueries = BuildSAMSeedQueries(SubjectSeeds, DescriptorSeeds);

     for (const auto& Query : SeedQueries)
     {
          AddTerm(Query, "query", "seed_query", 0.79, 0.82);
     }

     std::vector<std::string> ProfileTerms;

     {
          std::lock_guard<std::mutex> Lock(DBMutex);

          if (Database)
          {
               std::string RawProfile;
               const rocksdb::Status Status =
                    Database->Get(rocksdb::ReadOptions(), BuildCollectionProfileKey(Collection), &RawProfile);

               if (Status.ok() && !RawProfile.empty())
               {
                    try
                    {
                        const nlohmann::json Root = nlohmann::json::parse(RawProfile);

                        auto AppendProfileTerms = [&](const nlohmann::json& Array, const char* Field)
                        {
                             if (!Array.is_array())
                             {
                                  return;
                             }

                             for (const auto& Item : Array)
                             {
                                  if (!Item.is_object())
                                  {
                                       continue;
                                  }

                                  if (Field != nullptr)
                                  {
                                       const std::string Text = NormalizeTerm(Item.value(Field, ""));

                                       if (!Text.empty())
                                       {
                                            ProfileTerms.push_back(Text);
                                       }
                                  }

                                  for (const auto& NestedKey : {"aliases", "descriptors", "queries", "related"})
                                  {
                                       if (!Item.contains(NestedKey) || !Item[NestedKey].is_array())
                                       {
                                            continue;
                                       }

                                       for (const auto& Nested : Item[NestedKey])
                                       {
                                            if (Nested.is_string())
                                            {
                                                 const std::string Text = NormalizeTerm(Nested.get<std::string>());

                                                 if (!Text.empty())
                                                 {
                                                      ProfileTerms.push_back(Text);
                                                 }
                                            }
                                       }
                                  }
                             }
                        };

                        AppendProfileTerms(Root.value("terms", nlohmann::json::array()), "text");
                        AppendProfileTerms(Root.value("learned_terms", nlohmann::json::array()), "text");
                        AppendProfileTerms(Root.value("families", nlohmann::json::array()), "subject");
                        AppendProfileTerms(Root.value("learned_families", nlohmann::json::array()), "subject");
                    }
                    catch (...)
                    {
                    }
               }
          }
     }

     ProfileTerms = UniqueNormalizedPhrases(ProfileTerms, 18);

     std::string ContextError;

     for (const auto& Term : GenerateLLMTerms(Collection, Doc, &ContextError))
     {
          AddTerm(Term.Text, Term.Kind, Term.Source, Term.Score, Term.Signal);
     }

     for (const auto& Term : GenerateLLMTermsFromProfile(Collection, Doc, ProfileTerms, &ContextError))
     {
          AddTerm(Term.Text, Term.Kind, Term.Source, Term.Score, Term.Signal);
     }

     for (size_t Iteration = 0; Iteration < 2; ++Iteration)
     {
          const std::vector<std::string> CurrentSubjects = SubjectSeeds;
          const std::vector<std::string> CurrentDescriptors = DescriptorSeeds;
          const std::vector<std::string> CurrentQueries = QuerySeeds;

          for (const auto& SubjectSeed : CurrentSubjects)
          {
               const std::vector<std::string> SubjectTokens = NormalizeSAMTokens(SubjectSeed, true);

               if (SubjectTokens.empty())
               {
                    continue;
               }

               for (const auto& DescriptorSeed : CurrentDescriptors)
               {
                    if (ShouldSkipSAMPairPhrase(SubjectSeed, DescriptorSeed))
                    {
                         continue;
                    }

                    const std::vector<std::string> DescriptorTokens = NormalizeSAMTokens(DescriptorSeed, true);

                    if (DescriptorTokens.empty())
                    {
                         continue;
                    }

                    size_t Shared = 0;

                    for (const auto& Token : DescriptorTokens)
                    {
                         if (std::find(SubjectTokens.begin(), SubjectTokens.end(), Token) != SubjectTokens.end())
                         {
                              ++Shared;
                         }
                    }

                    if (Shared >= SubjectTokens.size() && Shared >= DescriptorTokens.size())
                    {
                         continue;
                    }

                    AddTerm(SubjectSeed + " " + DescriptorSeed,
                            "query",
                            Iteration == 0 ? "iterative_pair" : "iterative_refine",
                            Iteration == 0 ? 0.80 : 0.76,
                            Iteration == 0 ? 0.84 : 0.80);
               }

               for (const auto& QuerySeed : CurrentQueries)
               {
                    if (QuerySeed == SubjectSeed)
                    {
                         continue;
                    }

                    AddTerm(QuerySeed + " " + SubjectSeed,
                            "query",
                            Iteration == 0 ? "query_refine" : "query_expand",
                            Iteration == 0 ? 0.72 : 0.68,
                            Iteration == 0 ? 0.76 : 0.72);
               }
          }
     }

     std::vector<TermEntry> Terms = Collector.Finalize(96);

     if (Terms.empty())
     {
          if (!Subject.empty())
          {
               Terms.push_back(TermEntry{Subject, "subject", "title", 0.95, 0.95});
          }
          else if (ErrorMessage)
          {
               *ErrorMessage = "No useful document terms could be derived.";
          }
     }

     if (!ContextError.empty() && ErrorMessage && ErrorMessage->empty())
     {
          *ErrorMessage = ContextError;
     }

     return Terms;
}

/* Record one search idea and merge its supporting documents. */

#endif /* HLQUERY_SAM_SPLIT_INCLUDE */
