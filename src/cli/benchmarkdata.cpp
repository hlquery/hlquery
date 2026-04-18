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
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>
#include <vendor/json/json.hpp>

#include "benchmarkclient.h"
#include "utils/tools.h"

/* External declarations. */

extern bool verbose_mode;

/* Fake synonyms and stopwords data. */

static const std::vector<std::pair<std::string, std::vector<std::string>>> FAKE_SYNONYMS =
     {
          {"car", {"automobile", "vehicle", "auto", "motorcar"}},
          {"phone", {"mobile", "cellphone", "smartphone", "device"}},
          {"computer", {"pc", "laptop", "desktop", "machine"}},
          {"house", {"home", "residence", "dwelling", "abode"}},
          {"dog", {"puppy", "canine", "pet", "hound"}},
          {"cat", {"kitten", "feline", "pet", "kitty"}},
          {"book", {"novel", "tome", "volume", "publication"}},
          {"food", {"meal", "cuisine", "dish", "fare"}},
          {"water", {"liquid", "h2o", "aqua", "fluid"}},
          {"tree", {"plant", "sapling", "wood", "forest"}},
          {"music", {"rock", "jazz", "pop", "band", "artist"}},
          {"science", {"physics", "biology", "chemistry", "reSearch", "experiment"}},
          {"cake", {"pastry", "dessert", "sweet", "bakery", "chocolate"}}};

/* Generates a random phrase string. */

std::string GenerateRandomPhraseString(int doc_id, int thread_id, int phrase_num)
{
     std::mt19937 gen(doc_id * 10000 + thread_id * 100 + phrase_num);

     const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

     std::uniform_int_distribution<> dis(0, chars.size() - 1);

     std::string result;

     result.reserve(5);

     for (int i = 0; i < 5; i++)
     {
          result += chars[dis(gen)];
     }

     return result;
}

/* Generates content with synonyms and stopwords. */

std::string GenerateContentWithSynonymsAndStopwords(int doc_id, int thread_id, int col_idx, std::mt19937 &gen)
{
     std::string content = "This is the content of document " + std::to_string(doc_id) + " in collection " + std::to_string(col_idx) + " inserted by thread " + std::to_string(thread_id);

     std::uniform_real_distribution<> stopword_dist(0.0, 1.0);

     std::vector<std::string> included_stopwords;

     for (const auto &sw : FAKE_STOPWORDS)
     {
          if (stopword_dist(gen) < 0.3)
          {
               included_stopwords.push_back(sw);
          }
     }

     if (!included_stopwords.empty())
     {
          content += ". Common words: ";

          for (size_t i = 0; i < included_stopwords.size(); ++i)
          {
               if (i > 0)
               {
                    content += " ";
               }

               content += included_stopwords[i];
          }
     }

     std::uniform_int_distribution<> synonym_count_dist(1, 3);
     std::uniform_int_distribution<> synonym_idx_dist(0, FAKE_SYNONYMS.size() - 1);
     std::uniform_real_distribution<> use_root_dist(0.0, 1.0);

     int num_synonyms = synonym_count_dist(gen);

     std::set<int> used_indices;

     for (int i = 0; i < num_synonyms && used_indices.size() < FAKE_SYNONYMS.size(); ++i)
     {
          int syn_idx;

          do
          {
               syn_idx = synonym_idx_dist(gen);
          } while (used_indices.count(syn_idx) > 0);

          used_indices.insert(syn_idx);

          const auto &syn_pair = FAKE_SYNONYMS[syn_idx];

          content += ". ";

          if (use_root_dist(gen) < 0.4)
          {
               content += "Topic about " + syn_pair.first;
          }
          else
          {
               std::uniform_int_distribution<> syn_term_dist(0, syn_pair.second.size() - 1);

               int term_idx = syn_term_dist(gen);

               content += "Topic about " + syn_pair.second[term_idx];
          }
     }

     int unique_doc_id = col_idx * 1000000 + doc_id * 100 + thread_id;

     std::string phrase1 = GenerateRandomPhraseString(unique_doc_id, thread_id, 1);
     std::string phrase2 = GenerateRandomPhraseString(unique_doc_id, thread_id, 2);
     std::string phrase3 = GenerateRandomPhraseString(unique_doc_id, thread_id, 3);

     content += ". Unique Search phrases: " + phrase1 + " " + phrase2 + " " + phrase3 + ". Lorem ipsum dolor sit amet consectetur adipiscing elit.";

     return content;
}

/* Generates a random string. */

std::string GenerateRandomString(int length, std::mt19937 &gen)
{
     const std::string chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

     std::uniform_int_distribution<> dist(0, chars.size() - 1);

     std::string result;

     result.reserve(length);

     for (int i = 0; i < length; i++)
     {
          result += chars[dist(gen)];
     }

     return result;
}

/* Generates a random word. */

std::string GenerateRandomWord(std::mt19937 &gen)
{
     static const std::vector<std::string> words =
          {
               "product", "item", "article", "review", "comment", "post", "note", "document",
               "book", "movie", "song", "game", "app", "tool", "device", "machine",
               "system", "service", "platform", "application", "software", "hardware",
               "technology", "innovation", "solution", "feature", "function", "capability"};

     std::uniform_int_distribution<> dist(0, words.size() - 1);

     return words[dist(gen)];
}

/* Generates a random sentence. */

std::string GenerateRandomSentence(int word_count, std::mt19937 &gen)
{
     std::string sentence;

     for (int i = 0; i < word_count; i++)
     {
          if (i > 0)
          {
               sentence += " ";
          }

          sentence += GenerateRandomWord(gen);
     }

     return sentence;
}

/* Generates a random paragraph. */

std::string GenerateRandomParagraph(int sentence_count, std::mt19937 &gen)
{
     std::string paragraph;

     std::uniform_int_distribution<> sentence_length(5, 15);

     for (int i = 0; i < sentence_count; i++)
     {
          if (i > 0)
          {
               paragraph += " ";
          }

          paragraph += GenerateRandomSentence(sentence_length(gen), gen) + ".";
     }

     return paragraph;
}

/* Inserts sample documents into a collection. */

void InsertSampleDocuments(BenchmarkClient &client, const std::string &collection, int num_docs, bool verbose)
{
     nlohmann::json payload;

     payload["documents"] = nlohmann::json::array();

     static const std::map<std::string, std::vector<std::string>> REAL_NAMES =
          {
               {"products", {"Laptop Pro 15", "Smartphone X1", "Wireless Earbuds", "Smart Watch G2", "4K Monitor 27", "Mechanical Keyboard", "Gaming Mouse", "External SSD 1TB", "USB-C Hub", "Tablet Air", "VR Headset", "Smart Home Hub", "Noise Cancelling Headphones", "Bluetooth Speaker", "Action Camera", "Mirrorless Camera", "Dron X Pro", "Fitness Tracker", "E-Reader", "Power Bank", "Graphics Card", "Gaming Chair", "Dual Monitor Stand", "Mechanical Pencil", "Smart Pen", "Desk Lamp", "Ergonomic Keyboard", "Trackball Mouse", "Webcam 4K", "Microphone", "Internal SSD", "RAM Module", "Motherboard", "CPU Cooler", "PC Case", "Power Supply Unit", "Sound Card", "Capture Card", "NAS Drive", "Router WiFi 6", "Range Extender", "Network Switch", "Portable HDD", "Thermal Paste", "Cable Management Kit", "Screwdriver Set", "Anti-Static Mat", "Laptop Sleeve", "Backpack", "Cooling Pad"}},
               {"books", {"The Great Gatsby", "1984", "To Kill a Mockingbird", "The Catcher in the Rye", "Moby Dick", "Pride and Prejudice", "The Hobbit", "Ulysses", "War and Peace", "The Odyssey", "Crime and Punishment", "The Brothers Karamazov", "Anna Karenina", "Madame Bovary", "The Divine Comedy", "Paradise Lost", "The Iliad", "Aeneid", "Don Quixote", "Hamlet", "Macbeth", "Othello", "King Lear", "The Tempest", "Frankenstein", "Dracula", "Jane Eyre", "Wuthering Heights", "Great Expectations", "A Tale of Two Cities", "David Copperfield", "Oliver Twist", "Les Miserables", "The Hunchback of Notre Dame", "The Count of Monte Cristo", "The Three Musketeers", "Brave New World", "Fahrenheit 451", "Catch-22", "Slaughterhouse-Five", "The Grapes of Wrath", "East of Eden", "Of Mice and Men", "Heart of Darkness", "Lord of the Flies", "Animal Farm", "The Old Man and the Sea", "For Whom the Bell Tolls", "The Sun Also Rises", "A Farewell to Arms"}},
               {"music", {"Linkin Park", "Coldplay", "Muse", "Arctic Monkeys", "The Killers", "The Strokes", "Gorillaz", "Daft Punk", "Tame Impala", "Arcade Fire", "The White Stripes", "The Black Keys", "Twenty One Pilots", "Imagine Dragons", "Maroon 5", "OneRepublic", "Paramore", "Fall Out Boy", "Panic! At The Disco", "My Chemical Romance", "Blink-182", "Sum 41", "The Offspring", "Weezer", "Pixies", "Sonic Youth", "Pavement", "Smashing Pumpkins", "Nine Inch Nails", "Tool", "System of a Down", "Korn", "Slipknot", "Avenged Sevenfold", "Disturbed", "Breaking Benjamin", "Three Days Grace", "Shinedown", "Alter Bridge", "Seether", "Staind", "Puddle of Mudd", "Hoobastank", "Incubus", "Audioslave", "Velvet Revolver", "Them Crooked Vultures", "Queens of the Stone Age", "Beastie Boys", "Rammstein"}},
               {"movies", {"The Godfather", "The Shawshank Redemption", "Pulp Fiction", "The Dark Knight", "Forrest Gump", "Inception", "The Matrix", "Star Wars", "Goodfellas", "Seven Samurai", "City of God", "Life is Beautiful", "Spirited Away", "Parasite", "The Silence of the Lambs", "Saving Private Ryan", "The Green Mile", "The Prestige", "Gladiator", "The Departed", "Whiplash", "Interstellar", "The Lion King", "Back to the Future", "Terminator 2", "Alien", "Aliens", "Psycho", "Rear Window", "Vertigo", "North by Northwest", "Citizen Kane", "Casablanca", "Sunset Boulevard", "Some Like It Hot", "Double Indemnity", "The Third Man", "The Bridge on the River Kwai", "Lawrence of Arabia", "Doctor Zhivago", "2001: A Space Odyssey", "A Clockwork Orange", "The Shining", "Full Metal Jacket", "Blade Runner", "Raiders of the Lost Ark", "Jurassic Park", "Schindler's List", "The Pianist", "Braveheart"}},
               {"sports", {"World Cup Final", "Super Bowl LVIII", "Wimbledon Championship", "NBA Finals Game 7", "Tour de France", "Masters Tournament", "Olympic 100m Dash", "Stanley Cup", "World Series", "Daytona 500", "Indy 500", "Kentucky Derby", "Boston Marathon", "Ironman World Championship", "X Games", "The Open Championship", "Ryder Cup", "Davis Cup", "America's Cup", "Le Mans 24 Hours", "Tour down Under", "Giro d'Italia", "Vuelta a España", "Monaco Grand Prix", "British Grand Prix", "Italian Grand Prix", "Singapore Grand Prix", "Abu Dhabi Grand Prix", "Melbourne Cup", "Rugby World Cup", "Six Nations", "Cricket World Cup", "The Ashes", "Australian Open", "French Open", "US Open", "Copa America", "Euro 2024", "Champions League Final", "Copa Libertadores", "IPL Final", "Super Rugby", "NFL Draft", "NBA Draft", "MLB All-Star Game", "NHL All-Star Game", "World Baseball Classic", "Commonwealth Games", "Asian Games", "Pan American Games"}},
               {"technology", {"Artificial Intelligence", "Quantum Computing", "Blockchain Technology", "Cloud Computing", "Internet of Things", "Cybersecurity", "5G Networks", "Virtual Reality", "Edge Computing", "Big Data", "Machine Learning", "Augmented Reality", "Nanotechnology", "Biotechnology", "Renewable Energy", "Autonomous Vehicles", "Smart Grids", "Digital Twins", "3D Printing", "Robotics", "Generative AI", "Web3", "Metaverse", "Data Science", "DevOps", "Cyber-Physical Systems", "Smart Cities", "FinTech", "HealthTech", "EdTech", "Natural Language Processing", "Computer Vision", "Deep Learning", "Reinforcement Learning", "Graph Databases", "Serverless Computing", "Microservices", "Containerization", "API First", "Zero Trust", "Quantum Encryption", "Neuromorphic Computing", "Bio-hacking", "Space Exploration Tech", "Nuclear Fusion", "Graphene Electronics", "Solid-State Batteries", "Hyperloop", "Vertical Farming", "Lab-Grown Meat"}},
               {"art", {"Mona Lisa", "Starry Night", "The Last Supper", "The Scream", "Guernica", "Girl with a Pearl Earring", "The Birth of Venus", "Las Meninas", "Creation of Adam", "Water Lilies", "The Night Watch", "The Garden of Earthly Delights", "Liberty Leading the People", "The Kiss", "American Gothic", "The Persistence of Memory", "Nighthawks", "Composition with Red Blue and Yellow", "Campbell's Soup Cans", "The Son of Man", "A Sunday Afternoon on the Island of La Grande Jatte", "Whistler's Mother", "The Thinker", "The School of Athens", "Grande Odalisque", "Napoleon Crossing the Alps", "Venus de Milo", "David", "Pieta", "The Great Wave off Kanagawa", "Luncheon of the Boating Party", "The Arnolfini Portrait", "The Swing", "Impression Sunrise", "The Wanderer above the Sea of Fog", "Portrait of Madame X", "The Harvesters", "Breezing Up", "The Gross Clinic", "The Veteran in a New Field", "No. 5 1948", "Woman I", "Whaam!", "Drowning Girl", "The Treachery of Images", "Golconda", "The Empire of Light", "Self-Portrait with Thorn", "The Two Fridas", "The Broken Column", "Terracotta Army"}},
               {"food", {"Margherita Pizza", "Sushi Platter", "Pad Thai", "Beef Wellington", "Tacos Al Pastor", "Chicken Tikka Masala", "Croissant", "Dim Sum", "Hamburger Deluxe", "Peking Duck", "Paella", "Poutine", "Fish and Chips", "Goulash", "Schnitzel", "Pierogi", "Moussaka", "Falafel", "Hummus", "Shakshuka", "Bibimbap", "Kimchi", "Pho", "Banh Mi", "Satay", "Nasi Goreng", "Rendang", "Laksa", "Papadum", "Naan", "Samosa", "Pakora", "Biryani", "Kebab", "Baklava", "Gelato", "Tiramisu", "Cannoli", "Churros", "Flan", "Creme Brulee", "Apple Pie", "Cheesecake", "Brownies", "Cookies", "Cupcakes", "Donuts", "Bagels", "Pretzels", "Popcorn"}},
               {"history", {"WW1", "WW2", "WW3", "Industrial Revolution", "Magna Carta", "Fall of Rome", "Renaissance", "Cold War", "Enlightenment", "Meiji Restoration", "French Revolution", "American Revolution", "Russian Revolution", "Civil War", "Vietnam War", "Korean War", "Crusades", "Black Death", "Great Depression", "Space Race", "Digital Revolution", "Ancient Egypt", "Ancient Greece", "Aztec Empire", "Inca Empire", "Maya Civilization", "Ottoman Empire", "Mongol Empire", "British Empire", "Roman Empire", "Byzantine Empire", "Holy Roman Empire", "Viking Age", "Age of Discovery", "Scientific Revolution", "Golden Age of Piracy", "Victorian Era", "Roaring Twenties", "Great Fire of London", "Sinking of the Titanic", "Apollo 11", "Fall of the Berlin Wall", "Invention of the Printing Press", "Battle of Hastings", "Magna Carta Signing", "Boston Tea Party", "Signing of the Declaration of Independence", "French Revolution Starts", "Waterloo Battle", "Gettysburg Address"}},
               {"travel", {"Eiffel Tower", "Great Wall of China", "Machu Picchu", "Grand Canyon", "Taj Mahal", "Colosseum", "Statue of Liberty", "Pyramids of Giza", "Santorini", "Mount Fuji", "Louvre Museum", "Vatican City", "Acropolis of Athens", "Stonehenge", "Petra", "Angkor Wat", "Galapagos Islands", "Serengeti National Park", "Great Barrier Reef", "Sydney Opera House", "Niagara Falls", "Victoria Falls", "Iguazu Falls", "Banff National Park", "Yellowstone National Park", "Yosemite National Park", "Mount Everest", "Kilimanjaro", "Matterhorn", "Mont Blanc", "Venice Canals", "Amalfi Coast", "Santorini Oia", "Prague Old Town", "Big Ben", "Tower Bridge", "Burj Khalifa", "Golden Gate Bridge", "Christ the Redeemer", "Table Mountain", "Easter Island", "Alhambra", "Sagrada Familia", "Neuschwanstein Castle", "Versailles Palace", "Red Square", "Forbidden City", "Mecca", "Medina", "Jerusalem Old City"}},
               {"bands", {"The Beatles", "Led Zeppelin", "Pink Floyd", "Queen", "The Rolling Stones", "The Who", "Nirvana", "Radiohead", "The Doors", "Metallica", "AC/DC", "Guns N' Roses", "U2", "Aerosmith", "Black Sabbath", "Deep Purple", "Iron Maiden", "Judas Priest", "Motorhead", "Pantera", "Red Hot Chili Peppers", "Foo Fighters", "Green Day", "Pearl Jam", "Soundgarden", "Alice in Chains", "Stone Temple Pilots", "Rage Against the Machine", "Oasis", "Blur", "The Smiths", "Joy Division", "New Order", "The Cure", "Depeche Mode", "R.E.M.", "The Police", "Dire Straits", "Fleetwood Mac", "Eagles", "Bon Jovi", "Van Halen", "Def Leppard", "Motley Crue", "Poison", "Kiss", "Rush", "Genesis", "King Crimson", "Yes"}},
               {"cake", {"Chocolate Fudge", "Red Velvet", "New York Cheesecake", "Carrot Cake", "Lemon Drizzle", "Black Forest", "Victoria Sponge", "Tiramisu", "Strawberry Shortcake", "Angel Food", "Pound Cake", "Chiffon Cake", "Sponge Cake", "Genois", "Fruit Cake", "Rum Cake", "Coffee Cake", "Banana Bread", "Zucchini Bread", "Pumpkin Bread", "Marble Cake", "Bundt Cake", "Upside Down Cake", "Pineapple Upside Down", "Opera Cake", "Sachertorte", "Dobos Torte", "Gateau St. Honore", "Mille-Feuille", "Paris-Brest", "Profiteroles", "Eclairs", "Madeleines", "Macarons", "Cannoli", "Baklava", "Gulab Jamun", "Mochi", "Dorayaki", "Taiyaki", "Pavlova", "Lamingtons", "Trifle", "Bread and Butter Pudding", "Sticky Toffee Pudding", "Spotted Dick", "Christmas Pudding", "Hot Cross Buns", "Scones", "Shortbread"}},
               {"science", {"General Relativity", "Quantum Mechanics", "Evolution", "Double Helix", "Plate Tectonics", "Big Bang Theory", "Special Relativity", "Laws of Motion", "Germ Theory", "Periodic Table", "Standard Model", "Dark Matter", "Dark Energy", "Hubble's Law", "Kepler's Laws", "Maxwell's Equations", "Schrödinger's Equation", "Heisenberg Uncertainty Principle", "Pauli Exclusion Principle", "Entanglement", "String Theory", "M-Theory", "Quantum Field Theory", "Thermodynamics", "Statistical Mechanics", "Chaos Theory", "Fractals", "Cell Theory", "Genetics", "Epigenetics", "CRISPR", "Synthetic Biology", "Neuroscience", "Cognitive Science", "Astrophysics", "Cosmology", "Particle Physics", "Nuclear Physics", "Condensed Matter Physics", "Atomic Physics", "Molecular Biology", "Biochemistry", "Organic Chemistry", "Inorganic Chemistry", "Analytical Chemistry", "Physical Chemistry", "Geology", "Meteorology", "Oceanography", "Environmental Science"}},
          };

     for (int i = 1; i <= num_docs; i++)
     {
          nlohmann::json doc;

          std::string real_title = collection + " item " + std::to_string(i);

          std::string doc_id;

          if (REAL_NAMES.count(collection) > 0)
          {
               const auto &names = REAL_NAMES.at(collection);

               real_title = names[static_cast<size_t>(i - 1) % names.size()];

               if (static_cast<size_t>(num_docs) > names.size())
               {
                    real_title += " Vol. " + std::to_string(static_cast<size_t>(i - 1) / names.size() + 1);
               }

               std::string id_slug = real_title;

               std::transform(id_slug.begin(), id_slug.end(), id_slug.begin(), [](unsigned char c)
                              {
                                   return (std::isspace(c) || c == '.') ? '_' : std::tolower(c);
                              });

               id_slug.erase(std::remove_if(id_slug.begin(), id_slug.end(), [](unsigned char c)
                                            {
                                                 return !std::isalnum(c) && c != '_';
                                            }),
                             id_slug.end());

               doc_id = id_slug;

               if (static_cast<size_t>(num_docs) > names.size())
               {
                    doc_id += "_" + std::to_string(i);
               }
          }
          else
          {
               doc_id = std::to_string(1000 + i);
          }

          doc["id"] = doc_id;
          doc["title"] = real_title;

          if (collection == "products")
          {
               doc["description"] = "High-performance " + real_title + ".";
               doc["price"] = static_cast<float>(rand() % 5000) / 10.0f;
               doc["category"] = (i % 3 == 0) ? "Electronics" : "Home";
          }
          else if (collection == "books")
          {
               doc["author"] = "Author " + std::to_string(i % 5);
               doc["year"] = 1990 + (i % 35);
               doc["genre"] = (i % 2 == 0) ? "Fiction" : "Non-fiction";
          }
          else if (collection == "music")
          {
               doc["artist"] = real_title;
               doc["album"] = "Greatest Hits";
               doc["genre"] = (i % 3 == 0) ? "Rock" : ((i % 3 == 1) ? "Alternative" : "Pop");
               doc["content"] = "A legendary performance by " + real_title + " in the " + doc["genre"].get<std::string>() + " genre.";
          }
          else if (collection == "movies")
          {
               doc["director"] = "Director " + std::to_string(i % 5);
               doc["year"] = 1980 + (i % 45);
               doc["rating"] = static_cast<float>(rand() % 100) / 10.0f;
               doc["content"] = "A cinematic masterpiece directed by " + doc["director"].get<std::string>() + ".";
          }
          else if (collection == "sports")
          {
               doc["type"] = (i % 3 == 0) ? "Soccer" : ((i % 3 == 1) ? "Basketball" : "Tennis");
               doc["event"] = (i % 2 == 0) ? "Championship" : "Friendly Match";
               doc["content"] = "Exciting " + doc["type"].get<std::string>() + " " + doc["event"].get<std::string>() + " coverage.";
          }
          else if (collection == "technology")
          {
               doc["tech"] = real_title;
               doc["impact"] = "Revolutionary";
               doc["content"] = "Analysis of " + real_title + " and its transformative impact.";
          }
          else if (collection == "art")
          {
               doc["style"] = (i % 3 == 0) ? "Impressionism" : ((i % 3 == 1) ? "Surrealism" : "Abstract");
               doc["artist"] = "Artist " + std::to_string(i % 10);
               doc["content"] = "A beautiful " + doc["style"].get<std::string>() + " masterpiece created by " + doc["artist"].get<std::string>() + ".";
          }
          else if (collection == "food")
          {
               doc["cuisine"] = (i % 3 == 0) ? "Italian" : ((i % 3 == 1) ? "Japanese" : "Mexican");
               doc["dish"] = real_title;
               static const std::vector<std::vector<std::string>> ingredient_profiles = {
                    {"extra virgin olive oil", "sea salt", "garlic", "fresh basil", "grated parmesan"},
                    {"soy sauce", "sesame oil", "ginger", "scallions", "rice vinegar"},
                    {"lime juice", "cilantro", "chili flakes", "red onion", "cumin"},
                    {"butter", "heavy cream", "black pepper", "thyme", "shallots"},
                    {"tomato paste", "smoked paprika", "oregano", "coriander", "bay leaf"},
                    {"coconut milk", "turmeric", "curry powder", "garam masala", "fresh ginger"}};

               const auto &profile = ingredient_profiles[static_cast<size_t>(i) % ingredient_profiles.size()];
               std::string ingredients;
               for (size_t idx = 0; idx < profile.size(); ++idx)
               {
                    if (idx > 0)
                    {
                         ingredients += " | ";
                    }
                    ingredients += profile[idx];
               }
               doc["ingredients"] = ingredients;
               doc["content"] = "A delicious " + doc["cuisine"].get<std::string>() + " " + real_title + " recipe. Ingredients: " + ingredients + ".";
          }
          else if (collection == "history")
          {
               doc["period"] = (i % 3 == 0) ? "Ancient" : ((i % 3 == 1) ? "Medieval" : "Modern");
               doc["event"] = real_title;
               doc["content"] = "Analysis of " + real_title + " from the " + doc["period"].get<std::string>() + " era.";
          }
          else if (collection == "media")
          {
               doc["type"] = (i % 3 == 0) ? "Video" : ((i % 3 == 1) ? "Audio" : "Image");
               doc["format"] = (i % 3 == 0) ? "mp4" : ((i % 3 == 1) ? "mp3" : "jpg");
               doc["content"] = "A " + doc["type"].get<std::string>() + " file in " + doc["format"].get<std::string>() + " format.";
          }
          else if (collection == "travel")
          {
               doc["destination"] = real_title;
               doc["country"] = (i % 3 == 0) ? "France" : ((i % 3 == 1) ? "Japan" : "USA");
               doc["content"] = "Travel guide for " + real_title + " in " + doc["country"].get<std::string>() + ".";
          }
          else if (collection == "bands")
          {
               doc["name"] = real_title;
               doc["origin"] = (i % 2 == 0) ? "London" : "New York";
               doc["members"] = std::to_string(3 + (i % 4));
               doc["content"] = "A legendary band from " + doc["origin"].get<std::string>() + " with " + doc["members"].get<std::string>() + " members.";
          }
          else if (collection == "cake")
          {
               doc["type"] = (i % 2 == 0) ? "Chocolate" : "Vanilla";
               doc["flavor"] = (i % 3 == 0) ? "Sweet" : "Bitter";
               doc["content"] = "A delicious " + doc["type"].get<std::string>() + " cake with a " + doc["flavor"].get<std::string>() + " flavor.";
          }
          else if (collection == "science")
          {
               doc["field"] = (i % 3 == 0) ? "Physics" : ((i % 3 == 1) ? "Biology" : "Chemistry");
               doc["discovery"] = real_title;

               std::string jargon = (i % 3 == 0) ? "quantum entanglement" : ((i % 3 == 1) ? "gene editing" : "molecular bonds");

               doc["content"] = "Scientific paper discussing " + jargon + ". This is real scientific stuff.";
          }

          payload["documents"].push_back(doc);
     }

     std::string json_str = payload.dump();

     client.MakeRequest("POST", "/collections/" + collection + "/documents/import", json_str, 2);
}

/* Dumps all collections and their documents. */

void DumpAllCollections(const std::string &base_url, const std::string &auth_token)
{
     BenchmarkClient client(base_url, auth_token);

     std::string connection_error = client.TestConnection();

     if (!connection_error.empty())
     {
          std::cerr << "Note: " << connection_error << ".\n";
          return;
     }

     std::vector<std::string> collections = client.ListCollections();

     if (collections.empty())
     {
          std::cout << "No collections found.";
          return;
     }

     std::cout << "Dumping " << collections.size() << " collection(s)...\n\n";

     for (size_t i = 0; i < collections.size(); i++)
     {
          const std::string &collection_name = collections[i];

          std::cout << collection_name << "\n";

          HTTPResponse docs_response = client.GetCollectionDocuments(collection_name, 0, 10000);

          if (docs_response.StatusCode != 200)
          {
               std::cout << "  └─ Note: Could not get documents (HTTP " << docs_response.StatusCode << ").\n";

               if (i < collections.size() - 1)
               {
                    std::cout << "\n";
               }

               continue;
          }

          try
          {
               nlohmann::json docs_json = nlohmann::json::parse(docs_response.Body);

               if (!docs_json.contains("documents") || !docs_json["documents"].is_array())
               {
                    std::cout << "  └─ No documents found.\n";

                    if (i < collections.size() - 1)
                    {
                         std::cout << "\n";
                    }

                    continue;
               }

               auto documents = docs_json["documents"];

               size_t doc_count_val = documents.size();

               if (doc_count_val == 0)
               {
                    std::cout << "  └─ (empty).\n";
               }
               else
               {
                    std::cout << "  └─ " << doc_count_val << " document(s):\n";

                    for (size_t j = 0; j < documents.size(); j++)
                    {
                         const auto &doc = documents[j];

                         std::string prefix_str = "      ";

                         if (j == documents.size() - 1)
                         {
                              prefix_str = "      └─ ";
                         }
                         else
                         {
                              prefix_str = "      ├─ ";
                         }

                         std::string doc_id = "unknown";

                         if (doc.is_object() && doc.contains("id"))
                         {
                              doc_id = doc["id"].get<std::string>();
                         }
                         else if (doc.is_string())
                         {
                              doc_id = doc.get<std::string>();
                         }

                         std::cout << prefix_str << doc_id << "\n";

                         if (doc.is_object())
                         {
                              std::string doc_json_str = doc.dump(2);

                              std::istringstream iss(doc_json_str);

                              std::string line;

                              std::string indent_str = (j == documents.size() - 1) ? "         " : "      │  ";

                              while (std::getline(iss, line))
                              {
                                   std::cout << indent_str << line << "\n";
                              }
                         }
                         else
                         {
                              std::string indent_str = (j == documents.size() - 1) ? "         " : "      │  ";

                              std::cout << indent_str << doc.dump(2) << "\n";
                         }
                    }
               }
          }
          catch (const std::exception &e)
          {
               std::cout << "  └─ Note: Issue parsing documents: " << e.what() << ".\n";
          }

          if (i < collections.size() - 1)
          {
               std::cout << "\n";
          }
     }
}
