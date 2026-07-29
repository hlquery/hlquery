#!/usr/bin/env python3
"""Generate the public, synthetic data used by `hlquery-benchmark --fake`.

The generated records are deterministic and intentionally avoid claims about
real people, organizations, rankings, market activity, or current events.
"""

from __future__ import annotations

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "run" / "benchmark"

SAFE_NOTICE = (
    "Synthetic sample data for HLQuery demonstrations; it is not a factual "
    "claim about a real person, organization, event, or market."
)
MARKET_NOTICE = (
    "Synthetic demo scenario using no live market data; it is not investment, "
    "financial, legal, or trading advice."
)


def slug(value: str) -> str:
    normalized = re.sub(r"[^a-z0-9]+", "-", value.lower()).strip("-")
    return normalized or "sample"


def field(name: str, kind: str) -> dict[str, str]:
    return {"name": name, "type": kind}


def document(
    collection: str,
    title: str,
    content: str,
    *,
    labels: list[str],
    identifier: str | None = None,
    description: str | None = None,
    data_notice: str = SAFE_NOTICE,
    **extra: object,
) -> dict[str, object]:
    result: dict[str, object] = {
        "id": identifier or f"{collection}_{slug(title)}",
        "title": title,
        "content": content,
        "description": description or f"Synthetic {collection} demo record: {title}.",
        "labels": [collection, "demo", "synthetic", *labels],
        "data_notice": data_notice,
        "is_synthetic": True,
    }
    result.update(extra)
    return result


def fixture(
    collection: str,
    tags: list[str],
    documents: list[dict[str, object]],
    *,
    fields: list[dict[str, str]] | None = None,
    metadata: dict[str, str] | None = None,
    default_sorting_field: str | None = None,
    synonyms: list[dict[str, object]] | None = None,
    stopwords: list[str] | None = None,
) -> dict[str, object]:
    result: dict[str, object] = {
        "fixture_version": 2,
        "fixture_notice": SAFE_NOTICE,
        "collection": collection,
        "count": len(documents),
        "tags": tags,
        "documents": documents,
    }
    if fields:
        result["fields"] = fields
    if metadata:
        result["metadata"] = {
            "_fixture_kind": "synthetic_demo",
            "_fixture_notice": SAFE_NOTICE,
            **metadata,
        }
    else:
        result["metadata"] = {
            "_fixture_kind": "synthetic_demo",
            "_fixture_notice": SAFE_NOTICE,
        }
    if default_sorting_field:
        result["default_sorting_field"] = default_sorting_field
    if synonyms:
        result["synonyms"] = synonyms
    if stopwords:
        result["stopwords"] = stopwords
    return result


def write(name: str, payload: dict[str, object]) -> None:
    destination = OUTPUT / name
    destination.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def make_art() -> dict[str, object]:
    rows = [
        (
            "Tidal Memory No. 3 (Demo Artwork)",
            "Example Artist Aster Vale",
            "oil and cold wax on canvas",
            "contemporary abstraction",
            2021,
            "Layered blue and umber fields use scraped edges and repeated arcs to explore how coastal memories change over time.",
        ),
        (
            "Transit Lines at Dusk (Demo Artwork)",
            "Example Artist Rowan Pike",
            "screen print on paper",
            "urban graphic art",
            2019,
            "Overlapping route diagrams and warm color blocks turn an imagined evening commute into a study of rhythm and wayfinding.",
        ),
        (
            "The Listening Garden (Demo Sculpture)",
            "Example Artist Imani North",
            "recycled steel and ceramic",
            "environmental sculpture",
            2023,
            "Curved steel stems hold ceramic forms at different heights so wind and visitor movement continually alter the installation.",
        ),
        (
            "Archive of Small Windows (Demo Artwork)",
            "Example Artist Luca Fern",
            "photographic collage",
            "documentary collage",
            2020,
            "Fictional neighborhood photographs are arranged as a modular facade that invites viewers to compare private and public space.",
        ),
        (
            "Red Thread Assembly (Demo Installation)",
            "Example Artist Nia Sol",
            "cotton thread, wood, and light",
            "site-specific installation",
            2024,
            "A suspended grid of red thread casts shifting shadows and demonstrates how lighting can become part of a sculptural composition.",
        ),
        (
            "Quiet Machinery (Demo Artwork)",
            "Example Artist Theo March",
            "graphite and ink",
            "industrial drawing",
            2018,
            "Detailed fictional machine parts gradually dissolve into hand-drawn marks, contrasting technical precision with human gesture.",
        ),
        (
            "After the Rain Market (Demo Mural)",
            "Example Artist Esme Calder",
            "acrylic mural",
            "community muralism",
            2022,
            "A fictional market scene uses reflective pavement, bright produce, and multiple viewpoints to practice large-scale visual storytelling.",
        ),
        (
            "Folded Horizon (Demo Sculpture)",
            "Example Artist Omar Venn",
            "painted aluminum",
            "geometric sculpture",
            2021,
            "Angular planes change from green to gold as viewers move around the work, emphasizing perspective and negative space.",
        ),
        (
            "Portrait with Borrowed Light (Demo Artwork)",
            "Example Artist Hana Reed",
            "charcoal and pastel",
            "figurative portraiture",
            2023,
            "A fictional sitter is rendered with soft charcoal and a narrow band of pastel to examine focus, expression, and reflected light.",
        ),
        (
            "Common Ground Map (Demo Installation)",
            "Example Artist Mateo Lark",
            "paper, fabric, and projection",
            "participatory installation",
            2024,
            "Visitors can add fictional routes and landmarks to a projected map, creating a changing record of shared spatial imagination.",
        ),
    ]
    documents = [
        document(
            "art",
            title,
            f"{summary} The catalog note discusses medium, composition, installation requirements, and the intended gallery experience.",
            labels=[movement, medium, "artwork"],
            artist_name=artist,
            medium=medium,
            movement=movement,
            year=year,
            exhibition_context="fictional public gallery study",
        )
        for title, artist, medium, movement, year, summary in rows
    ]
    return fixture(
        "art",
        ["painting", "sculpture", "printmaking", "collage", "installation", "drawing", "mural", "portrait"],
        documents,
        fields=[
            field("artist_name", "string"),
            field("medium", "string"),
            field("movement", "string"),
            field("year", "int32"),
            field("exhibition_context", "string"),
        ],
        synonyms=[
            {
                "id": "art_syn_artwork",
                "root": "artwork",
                "synonyms": ["piece", "work of art", "creative work", "visual work"],
            }
        ],
    )


def make_people() -> dict[str, object]:
    first_names = ["Avery", "Bianca", "Caleb", "Dara", "Elias", "Farah", "Galen", "Helena", "Isaac", "Jun"]
    middle_names = ["Alexis", "Brooke", "Cameron", "Drew", "Emery", "Francis", "Gray", "Harper", "Indigo", "Jordan"]
    last_names = ["Northwind", "Cedar", "Marrow", "Solis", "Kestrel", "Vale", "Rowan", "Pike", "Lark", "Fern"]
    roles = [
        ("community archivist", "neighborhood history and accessible archives"),
        ("software reliability engineer", "observability, incident learning, and developer mentoring"),
        ("urban mobility planner", "safe walking routes and reliable public transit"),
        ("science educator", "hands-on astronomy and evidence-based classroom projects"),
        ("museum programs curator", "community exhibitions and inclusive interpretation"),
        ("cooperative business adviser", "local entrepreneurship and practical budgeting"),
        ("public health data analyst", "community wellness dashboards and data literacy"),
        ("civil infrastructure engineer", "resilient bridges and sustainable materials"),
        ("documentary editor", "visual storytelling and public-interest media"),
        ("food culture writer", "seasonal cooking and community recipe archives"),
    ]
    locations = [
        ("Portland", "Oregon", 45.5152, -122.6784),
        ("Austin", "Texas", 30.2672, -97.7431),
        ("Chicago", "Illinois", 41.8781, -87.6298),
        ("Atlanta", "Georgia", 33.7490, -84.3880),
        ("Seattle", "Washington", 47.6062, -122.3321),
        ("Denver", "Colorado", 39.7392, -104.9903),
        ("Boston", "Massachusetts", 42.3601, -71.0589),
        ("Phoenix", "Arizona", 33.4484, -112.0740),
        ("Minneapolis", "Minnesota", 44.9778, -93.2650),
        ("San Diego", "California", 32.7157, -117.1611),
    ]
    organizations = [
        "Example Cedar Archive",
        "Example Harbor Software Cooperative",
        "Example Open Streets Studio",
        "Example Night Sky Learning Lab",
        "Example Common Gallery",
        "Example Market Works Cooperative",
        "Example Community Data Lab",
        "Example Resilient Works",
        "Example Northlight Media",
        "Example Seasonal Table Journal",
    ]
    documents: list[dict[str, object]] = []
    for index in range(100):
        first = first_names[index % len(first_names)]
        middle = middle_names[(index // 10) % len(middle_names)]
        last = last_names[(index * 3 + index // 10) % len(last_names)]
        role, interests = roles[(index * 7) % len(roles)]
        city, state, latitude, longitude = locations[(index * 3 + 2) % len(locations)]
        organization = organizations[(index * 7) % len(organizations)]
        full_name = f"{first} {middle} {last}"
        title = f"{full_name} — Fictional Demo Profile"
        biography = (
            f"{full_name} is a fictional {role} based in {city}, {state}. "
            f"This synthetic profile describes work at {organization} involving {interests}. "
            "It exists only to demonstrate name, biography, occupation, and location search."
        )
        documents.append(
            document(
                "people",
                title,
                biography,
                identifier=f"people_profile_{index + 1:03d}",
                labels=[role, city, state, "fictional-person"],
                first_name=first,
                middle_name=middle,
                last_name=last,
                full_name=full_name,
                biography=biography,
                occupation=role,
                city=city,
                state=state,
                country="United States",
                interests=interests,
                organization=organization,
                profile_type="fictional_demo_person",
                location=[latitude, longitude],
                location_name=f"Fictional profile location near {city}, {state}",
            )
        )
    return fixture(
        "people",
        [role for role, _ in roles],
        documents,
        fields=[
            field("first_name", "string"),
            field("middle_name", "string"),
            field("last_name", "string"),
            field("full_name", "string"),
            field("biography", "string"),
            field("occupation", "string"),
            field("city", "string"),
            field("state", "string"),
            field("country", "string"),
            field("interests", "string"),
            field("organization", "string"),
            field("profile_type", "string"),
        ],
        synonyms=[
            {
                "id": "people_syn_biography",
                "root": "biography",
                "synonyms": ["profile", "background", "bio", "life summary"],
            }
        ],
    )


def make_universities() -> dict[str, object]:
    roots = [
        "Redwood Valley",
        "Great Lakes",
        "Desert Sky",
        "Atlantic Harbor",
        "Prairie Ridge",
        "Blue River",
        "Cedar Grove",
        "North Coast",
        "Golden Mesa",
        "Lakeview",
        "High Plains",
        "Copper Canyon",
        "Silver Oak",
        "Pine Harbor",
        "Sunrise Valley",
        "Granite Hill",
        "Maple Coast",
        "Riverbend",
        "Summit Fields",
        "Coastal Prairie",
        "Juniper Bay",
        "Orchard Ridge",
        "Clearbrook",
        "Horizon Plains",
        "Willow Creek",
    ]
    suffixes = [
        ("University", "fictional_private_research_university"),
        ("Institute of Technology", "fictional_technical_institute"),
        ("College of Arts and Sciences", "fictional_liberal_arts_college"),
        ("Polytechnic University", "fictional_public_polytechnic"),
    ]
    campuses = [
        ("Cambridge", "Massachusetts", 42.3736, -71.1097, "New England"),
        ("Palo Alto", "California", 37.4419, -122.1430, "West Coast"),
        ("Austin", "Texas", 30.2672, -97.7431, "Southwest"),
        ("Seattle", "Washington", 47.6062, -122.3321, "Pacific Northwest"),
        ("Ann Arbor", "Michigan", 42.2808, -83.7430, "Great Lakes"),
        ("Chicago", "Illinois", 41.8781, -87.6298, "Midwest"),
        ("Atlanta", "Georgia", 33.7490, -84.3880, "Southeast"),
        ("Denver", "Colorado", 39.7392, -104.9903, "Mountain West"),
        ("Boston", "Massachusetts", 42.3601, -71.0589, "New England"),
        ("Minneapolis", "Minnesota", 44.9778, -93.2650, "Upper Midwest"),
        ("Phoenix", "Arizona", 33.4484, -112.0740, "Southwest"),
        ("Portland", "Oregon", 45.5152, -122.6784, "Pacific Northwest"),
        ("Philadelphia", "Pennsylvania", 39.9526, -75.1652, "Mid-Atlantic"),
        ("Raleigh", "North Carolina", 35.7796, -78.6382, "Southeast"),
        ("Madison", "Wisconsin", 43.0731, -89.4012, "Great Lakes"),
        ("Salt Lake City", "Utah", 40.7608, -111.8910, "Mountain West"),
        ("New York", "New York", 40.7128, -74.0060, "Northeast"),
        ("Columbus", "Ohio", 39.9612, -82.9988, "Midwest"),
        ("Nashville", "Tennessee", 36.1627, -86.7816, "Southeast"),
        ("San Diego", "California", 32.7157, -117.1611, "West Coast"),
        ("Pittsburgh", "Pennsylvania", 40.4406, -79.9959, "Appalachia"),
        ("Miami", "Florida", 25.7617, -80.1918, "Southeast"),
        ("Albuquerque", "New Mexico", 35.0844, -106.6504, "Southwest"),
        ("Kansas City", "Missouri", 39.0997, -94.5786, "Central United States"),
        ("Burlington", "Vermont", 44.4759, -73.2121, "New England"),
    ]
    focus_pairs = [
        ("computer science", "digital humanities"),
        ("renewable energy", "materials engineering"),
        ("public health", "community data"),
        ("studio art", "museum studies"),
        ("environmental science", "public policy"),
        ("business analytics", "entrepreneurship"),
        ("education", "learning technology"),
        ("civil engineering", "urban planning"),
        ("biology", "applied mathematics"),
        ("music technology", "media production"),
    ]
    documents: list[dict[str, object]] = []
    for index in range(100):
        root = roots[index % len(roots)]
        suffix, institution_type = suffixes[index // len(roots)]
        city, state, latitude, longitude, region = campuses[(index * 7) % len(campuses)]
        focus_a, focus_b = focus_pairs[(index * 3) % len(focus_pairs)]
        rank = index + 1
        composite_score = round(99.5 - index * 0.37, 2)
        research_score = round(62.0 + ((97 - index * 7) % 38), 1)
        teaching_score = round(64.0 + ((91 - index * 5) % 36), 1)
        arts_score = round(60.0 + ((89 - index * 11) % 40), 1)
        student_experience_score = round(63.0 + ((93 - index * 13) % 37), 1)
        institution_name = f"Example {root} {suffix}"
        title = f"{institution_name} — Fictional U.S. University"
        focus_areas = f"{focus_a} | {focus_b}"
        content = (
            f"{institution_name} is a fictional institution located in {city}, {state}, United States. "
            f"Its demo profile emphasizes {focus_a} and {focus_b}, plus sample information about admissions, "
            "student services, research programs, libraries, campus arts, and community partnerships. "
            f"The synthetic U.S. demo ranking is {rank} of 100 with a composite score of {composite_score}; "
            "these values are generated solely to demonstrate filtering, sorting, faceting, and ranked search."
        )
        documents.append(
            document(
                "universities",
                title,
                content,
                identifier=f"universities_demo_{rank:03d}",
                labels=[state, city, region, institution_type, focus_a, focus_b, "fictional-university"],
                institution_name=institution_name,
                city=city,
                state=state,
                country="United States",
                region=region,
                city_aliases=f"{city} | {state} | {region} | United States",
                institution_type=institution_type,
                focus_areas=focus_areas,
                demo_rank=rank,
                rank=rank,
                rank_signal=round((101 - rank) / 100, 2),
                composite_score=composite_score,
                research_score=research_score,
                teaching_score=teaching_score,
                arts_score=arts_score,
                student_experience_score=student_experience_score,
                student_count=2400 + ((index * 977) % 28000),
                rank_source="HLQuery synthetic demo generator",
                rank_scope="100 fictional institutions in a United States demo dataset",
                rank_edition="2026 synthetic demo edition",
                rank_notice="Fictional ranking for software demonstration only; it does not evaluate or represent any real institution.",
                location=[latitude, longitude],
                location_name=f"Fictional campus near {city}, {state}",
            )
        )
    return fixture(
        "universities",
        ["united states", "university", "college", "campus", "research", "teaching", "arts", "admissions", "fictional ranking"],
        documents,
        fields=[
            field("institution_name", "string"),
            field("state", "string"),
            field("city", "string"),
            field("country", "string"),
            field("region", "string"),
            field("city_aliases", "string"),
            field("institution_type", "string"),
            field("focus_areas", "string"),
            field("demo_rank", "int32"),
            field("rank", "int32"),
            field("rank_signal", "float"),
            field("composite_score", "float"),
            field("research_score", "float"),
            field("teaching_score", "float"),
            field("arts_score", "float"),
            field("student_experience_score", "float"),
            field("student_count", "int32"),
            field("rank_source", "string"),
            field("rank_scope", "string"),
            field("rank_edition", "string"),
            field("rank_notice", "string"),
        ],
        default_sorting_field="demo_rank",
        metadata={
            "_rank_field": "demo_rank",
            "_rank_order": "asc",
            "_rank_scope": "fictional_united_states_demo",
            "_rank_edition": "2026 synthetic demo edition",
            "_rank_methodology": "deterministic generated composite score for software demonstration only",
        },
        synonyms=[
            {
                "id": "universities_syn_university",
                "root": "university",
                "synonyms": ["college", "campus", "institution", "school"],
            }
        ],
    )


def make_curated_collection(
    collection: str,
    tags: list[str],
    rows: list[tuple[str, str, dict[str, object]]],
    fields: list[dict[str, str]],
    *,
    synonyms: list[dict[str, object]] | None = None,
) -> dict[str, object]:
    documents = [
        document(
            collection,
            title,
            content,
            labels=[tags[index % len(tags)], *[str(value) for value in extra.values() if isinstance(value, str)][:2]],
            **extra,
        )
        for index, (title, content, extra) in enumerate(rows)
    ]
    return fixture(collection, tags, documents, fields=fields, synonyms=synonyms)


def make_books() -> dict[str, object]:
    rows = [
        ("The Lantern Index (Demo Novel)", "A fictional archivist discovers that a city library catalog records memories as well as books, creating a mystery about ownership and forgetting.", {"author_name": "Example Author Mira North", "genre": "literary mystery", "format": "hardcover", "publication_year": 2021}),
        ("Orbiting Home (Demo Novel)", "A fictional engineer on a long research mission records voice letters about distance, family, and the meaning of returning home.", {"author_name": "Example Author Theo Vale", "genre": "science fiction", "format": "paperback", "publication_year": 2022}),
        ("Small Bridges (Demo Essays)", "Ten fictional essays examine neighborhood infrastructure, mutual aid, and the everyday systems that connect communities.", {"author_name": "Example Author Lena Pike", "genre": "essay collection", "format": "ebook", "publication_year": 2020}),
        ("The Orchard Cipher (Demo Novel)", "Two siblings decode an invented set of botanical notes while deciding what to preserve from their family farm.", {"author_name": "Example Author Omar Reed", "genre": "family mystery", "format": "audiobook", "publication_year": 2023}),
        ("Weather for Beginners (Demo Guide)", "A fictional field guide explains clouds, pressure, local observation, and how to keep a careful weather journal.", {"author_name": "Example Author Hana Sol", "genre": "popular science", "format": "paperback", "publication_year": 2024}),
        ("Museum After Midnight (Demo Novel)", "A night guard and a conservator solve a fictional gallery puzzle using provenance notes, pigments, and patient observation.", {"author_name": "Example Author Caleb Fern", "genre": "cozy mystery", "format": "hardcover", "publication_year": 2019}),
        ("Recipes from an Imagined Coast (Demo Cookbook)", "A fictional cookbook pairs seasonal recipes with short notes about community kitchens and ingredient substitutions.", {"author_name": "Example Author Esme Lark", "genre": "cookbook", "format": "hardcover", "publication_year": 2022}),
        ("The Quiet Compiler (Demo Novel)", "A programmer maintains an old fictional language while mentoring a team through a difficult and humane software migration.", {"author_name": "Example Author Jun Marrow", "genre": "workplace fiction", "format": "ebook", "publication_year": 2024}),
        ("River Map Classroom (Demo Nonfiction)", "A fictional teacher uses maps, oral history, and water sampling to build an interdisciplinary local-history course.", {"author_name": "Example Author Farah Cedar", "genre": "education", "format": "paperback", "publication_year": 2021}),
        ("A Field Guide to Possible Futures (Demo Stories)", "Linked fictional stories explore repair, climate adaptation, public technology, and optimistic civic imagination.", {"author_name": "Example Author Avery Kestrel", "genre": "speculative stories", "format": "audiobook", "publication_year": 2023}),
    ]
    return make_curated_collection(
        "books",
        ["novel", "fiction", "essays", "mystery", "science", "cookbook", "technology", "education", "stories"],
        rows,
        [field("author_name", "string"), field("genre", "string"), field("format", "string"), field("publication_year", "int32")],
        synonyms=[{"id": "books_syn_book", "root": "book", "synonyms": ["novel", "volume", "title", "publication"]}],
    )


def make_music() -> dict[str, object]:
    rows = [
        ("Northbound Signals (Demo Album)", "A fictional electronic-jazz album built from brushed drums, warm synthesizers, and melodies that return in altered forms.", {"artist_name": "Example Ensemble Signal Garden", "genre": "electronic jazz", "release_type": "album", "release_year": 2023}),
        ("Paper Constellations (Demo EP)", "Five fictional chamber-pop songs use strings, close harmonies, and compact arrangements about memory and maps.", {"artist_name": "Example Artist Lina Vale", "genre": "chamber pop", "release_type": "EP", "release_year": 2022}),
        ("Common Frequency (Demo Live Set)", "A fictional live recording captures call-and-response vocals, improvised guitar, and audience percussion.", {"artist_name": "Example Band Common Frequency", "genre": "indie rock", "release_type": "live album", "release_year": 2024}),
        ("Glass River Suite (Demo Composition)", "A fictional four-movement suite shifts between solo piano, woodwinds, and quiet field recordings.", {"artist_name": "Example Composer Theo North", "genre": "contemporary classical", "release_type": "suite", "release_year": 2021}),
        ("Market Day Radio (Demo Playlist)", "A fictional playlist moves from upbeat soul to relaxed instrumental tracks for a community market broadcast.", {"artist_name": "Example Radio Cedar FM", "genre": "eclectic soul", "release_type": "playlist", "release_year": 2024}),
        ("Soft Circuit (Demo Album)", "A fictional ambient record combines modular synthesizer patterns with acoustic percussion and long dynamic fades.", {"artist_name": "Example Artist Hana Reed", "genre": "ambient electronic", "release_type": "album", "release_year": 2020}),
        ("Second Avenue Stories (Demo Album)", "Ten fictional hip-hop tracks use character sketches and detailed neighborhood production without referring to real events.", {"artist_name": "Example Artist Marrow Lane", "genre": "narrative hip-hop", "release_type": "album", "release_year": 2023}),
        ("Orchard Sessions (Demo EP)", "A fictional folk trio records close vocal harmonies and acoustic arrangements in a small studio setting.", {"artist_name": "Example Trio Orchard Sessions", "genre": "contemporary folk", "release_type": "EP", "release_year": 2019}),
        ("Blue Hour Transit (Demo Single)", "A fictional synth-pop single uses a steady bass line, layered vocals, and a bridge that briefly changes meter.", {"artist_name": "Example Artist Imani Pike", "genre": "synth pop", "release_type": "single", "release_year": 2024}),
        ("Rhythm Atlas (Demo Album)", "A fictional percussion-led album explores odd meters, ensemble dialogue, and spacious production.", {"artist_name": "Example Collective Rhythm Atlas", "genre": "instrumental fusion", "release_type": "album", "release_year": 2022}),
    ]
    return make_curated_collection(
        "music",
        ["album", "artist", "live", "studio", "playlist", "composition", "jazz", "rock", "pop", "folk"],
        rows,
        [field("artist_name", "string"), field("genre", "string"), field("release_type", "string"), field("release_year", "int32")],
        synonyms=[{"id": "music_syn_music", "root": "music", "synonyms": ["song", "melody", "track", "recording"]}],
    )


def make_movies() -> dict[str, object]:
    rows = [
        ("The Last Tram Home (Demo Film)", "A fictional drama follows five passengers whose delayed night tram turns into a quiet portrait of a changing city.", {"director_name": "Example Director Mira Sol", "genre": "drama", "runtime_minutes": 104, "release_year": 2022}),
        ("Signal at Low Tide (Demo Film)", "A fictional coastal mystery centers on a radio operator, a missing logbook, and clues revealed by the tide schedule.", {"director_name": "Example Director Omar Vale", "genre": "mystery", "runtime_minutes": 112, "release_year": 2023}),
        ("Kitchen Table Galaxy (Demo Film)", "A fictional family comedy uses an ambitious school astronomy project to bring three generations into the same room.", {"director_name": "Example Director Hana Fern", "genre": "family comedy", "runtime_minutes": 96, "release_year": 2021}),
        ("Archive Room Seven (Demo Film)", "A fictional historical thriller follows conservators who uncover contradictory records in an invented municipal archive.", {"director_name": "Example Director Caleb Pike", "genre": "historical thriller", "runtime_minutes": 118, "release_year": 2024}),
        ("Parallel Sidewalks (Demo Film)", "A fictional ensemble story follows an urban planner and a street photographer documenting the same neighborhood from different angles.", {"director_name": "Example Director Nia Cedar", "genre": "ensemble drama", "runtime_minutes": 109, "release_year": 2020}),
        ("Cloud Workshop (Demo Animation)", "A fictional animated adventure turns weather instruments into characters who learn how observation and cooperation improve a forecast.", {"director_name": "Example Director Jun Lark", "genre": "animation", "runtime_minutes": 88, "release_year": 2023}),
        ("The Repair Cafe (Demo Documentary)", "A fictional documentary profile explores skills sharing, product repair, and the social life of a monthly community workshop.", {"director_name": "Example Director Farah Reed", "genre": "documentary", "runtime_minutes": 84, "release_year": 2022}),
        ("Midnight Release (Demo Film)", "A fictional workplace comedy follows a software team trying to fix a harmless launch problem before sunrise.", {"director_name": "Example Director Theo Marrow", "genre": "workplace comedy", "runtime_minutes": 101, "release_year": 2024}),
        ("Garden Wall Frequency (Demo Film)", "A fictional science-fiction story uses a strange garden radio signal to examine curiosity, evidence, and friendship.", {"director_name": "Example Director Esme North", "genre": "science fiction", "runtime_minutes": 115, "release_year": 2021}),
        ("One More Rehearsal (Demo Film)", "A fictional music drama follows a community orchestra balancing ambition, care, and the pressure of opening night.", {"director_name": "Example Director Avery Kestrel", "genre": "music drama", "runtime_minutes": 107, "release_year": 2019}),
    ]
    return make_curated_collection(
        "movies",
        ["film", "cinema", "director", "cast", "mystery", "drama", "comedy", "animation", "documentary", "science fiction"],
        rows,
        [field("director_name", "string"), field("genre", "string"), field("runtime_minutes", "int32"), field("release_year", "int32")],
    )


def make_food() -> dict[str, object]:
    rows = [
        ("Roasted Tomato Farro Bowl (Demo Recipe)", "Roast tomatoes and garlic, fold them into warm farro, then finish with basil, lemon, and toasted seeds for contrasting texture.", {"dish": "roasted tomato farro bowl", "cuisine": "Mediterranean-inspired demo", "ingredients": "farro | tomato | garlic | basil | lemon | pumpkin seeds", "prep_minutes": 35}),
        ("Ginger Sesame Noodle Salad (Demo Recipe)", "Toss chilled noodles with cucumber, carrot, scallion, and a ginger-sesame dressing; add herbs immediately before serving.", {"dish": "ginger sesame noodle salad", "cuisine": "East Asian-inspired demo", "ingredients": "noodles | cucumber | carrot | ginger | sesame | scallion", "prep_minutes": 25}),
        ("Smoky Black Bean Tacos (Demo Recipe)", "Warm black beans with cumin and smoked paprika, then assemble with cabbage, lime, and a mild avocado sauce.", {"dish": "smoky black bean tacos", "cuisine": "Mexican-inspired demo", "ingredients": "black beans | corn tortillas | cabbage | lime | avocado | cumin", "prep_minutes": 30}),
        ("Herbed Mushroom Hand Pies (Demo Recipe)", "Cook mushrooms and shallots until dry, mix with herbs, and seal the cooled filling inside small pastry rounds.", {"dish": "herbed mushroom hand pies", "cuisine": "European-inspired demo", "ingredients": "mushrooms | shallot | thyme | pastry | black pepper", "prep_minutes": 55}),
        ("Coconut Lentil Stew (Demo Recipe)", "Simmer red lentils with coconut milk, tomato, turmeric, and ginger until creamy; serve with greens and rice.", {"dish": "coconut lentil stew", "cuisine": "South Asian-inspired demo", "ingredients": "red lentils | coconut milk | tomato | turmeric | ginger | spinach", "prep_minutes": 40}),
        ("Apple Oat Crumble (Demo Recipe)", "Bake sliced apples beneath an oat, cinnamon, and brown-sugar topping until the fruit bubbles and the surface turns crisp.", {"dish": "apple oat crumble", "cuisine": "home baking demo", "ingredients": "apples | oats | flour | cinnamon | butter | brown sugar", "prep_minutes": 50}),
        ("Charred Corn Street Salad (Demo Recipe)", "Combine charred corn with radish, herbs, lime, and a creamy dressing for a bright side dish with gentle heat.", {"dish": "charred corn street salad", "cuisine": "street-food-inspired demo", "ingredients": "corn | radish | cilantro | lime | yogurt | chili", "prep_minutes": 20}),
        ("Lemon Pea Risotto (Demo Recipe)", "Add warm stock gradually to rice, then fold in peas, lemon zest, herbs, and grated hard cheese off the heat.", {"dish": "lemon pea risotto", "cuisine": "Italian-inspired demo", "ingredients": "rice | peas | lemon | vegetable stock | parsley | parmesan", "prep_minutes": 45}),
        ("Miso Glazed Eggplant (Demo Recipe)", "Roast scored eggplant until tender, brush with a sweet-savory miso glaze, and broil briefly for caramelization.", {"dish": "miso glazed eggplant", "cuisine": "Japanese-inspired demo", "ingredients": "eggplant | miso | rice vinegar | sesame oil | scallion", "prep_minutes": 35}),
        ("Seasonal Citrus Tart (Demo Recipe)", "Fill a baked tart shell with citrus curd and arrange mixed citrus segments on top just before service.", {"dish": "seasonal citrus tart", "cuisine": "pastry demo", "ingredients": "flour | butter | eggs | lemon | orange | sugar", "prep_minutes": 75}),
    ]
    return make_curated_collection(
        "food",
        ["recipe", "cuisine", "restaurant", "dessert", "spice", "vegan", "street food", "seasonal"],
        rows,
        [field("ingredients", "string"), field("cuisine", "string"), field("dish", "string"), field("prep_minutes", "int32")],
    )


def make_science() -> dict[str, object]:
    rows = [
        ("Measuring Urban Heat Islands (Demo Research Brief)", "A synthetic field-study design compares shaded and unshaded temperature sensors while controlling for time, surface material, and instrument calibration.", {"field": "environmental science", "method": "paired field measurements", "evidence_level": "instructional example"}),
        ("Plant Growth under Different Light Colors (Demo Experiment)", "A classroom experiment varies light wavelength while keeping water, soil, temperature, and plant age consistent; repeated measurements reduce noise.", {"field": "biology", "method": "controlled growth experiment", "evidence_level": "instructional example"}),
        ("Tracking a Pendulum Period (Demo Experiment)", "Repeated timing at several string lengths illustrates measurement error and the square-root relationship between length and pendulum period.", {"field": "physics", "method": "repeated timing measurements", "evidence_level": "instructional example"}),
        ("Mapping a Simple Watershed (Demo Field Note)", "Elevation, drainage direction, and observations after rainfall are combined to explain how water moves through a small fictional watershed.", {"field": "earth science", "method": "mapping and observation", "evidence_level": "instructional example"}),
        ("Testing Vitamin C with Titration (Demo Lab)", "A synthetic lab compares juice samples using the same indicator, volumes, endpoint definition, and replicate procedure.", {"field": "chemistry", "method": "comparative titration", "evidence_level": "instructional example"}),
        ("Classifying Galaxy Shapes (Demo Data Study)", "A sample image set is labeled by multiple reviewers, then compared to discuss agreement, bias, and uncertainty in visual classification.", {"field": "astronomy", "method": "image classification", "evidence_level": "instructional example"}),
        ("Simulating Disease Spread without Personal Data (Demo Model)", "A toy network model varies contact rate and recovery time to show how assumptions change outcomes; it does not predict a real outbreak.", {"field": "epidemiology", "method": "synthetic network simulation", "evidence_level": "toy model"}),
        ("Replicating a Memory Recall Task (Demo Study)", "A fictional protocol randomizes word-list order and records aggregate recall to demonstrate preregistration, controls, and replication.", {"field": "cognitive science", "method": "randomized recall task", "evidence_level": "instructional example"}),
        ("Comparing Battery Discharge Curves (Demo Engineering Test)", "Synthetic voltage readings under fixed loads demonstrate sampling intervals, repeat trials, and cautious comparison of storage behavior.", {"field": "materials engineering", "method": "controlled discharge test", "evidence_level": "synthetic measurements"}),
        ("Finding Patterns in Noisy Sensor Data (Demo Analysis)", "A generated dataset is cleaned, visualized, and checked for outliers before a simple model is evaluated on held-out samples.", {"field": "data science", "method": "synthetic data analysis", "evidence_level": "toy dataset"}),
    ]
    return make_curated_collection(
        "science",
        ["research", "experiment", "physics", "biology", "chemistry", "astronomy", "fieldwork", "replication", "data"],
        rows,
        [field("field", "string"), field("method", "string"), field("evidence_level", "string")],
    )


def make_history() -> dict[str, object]:
    rows = [
        ("Reading a City Council Ledger (Demo History Guide)", "This instructional note explains how dates, authorship, omissions, and administrative purpose shape interpretation of a municipal ledger.", {"period": "modern", "source_type": "administrative record", "analysis_focus": "source context"}),
        ("Comparing Maps across Decades (Demo History Guide)", "A map-comparison exercise checks scale, labels, boundaries, and intended audience before drawing conclusions about urban change.", {"period": "nineteenth to twentieth century", "source_type": "historical maps", "analysis_focus": "spatial change"}),
        ("Building an Oral History Index (Demo Archive Note)", "A fictional archive workflow records consent, interview context, topics, and access limits while preserving the speaker's wording.", {"period": "contemporary", "source_type": "oral history", "analysis_focus": "ethical description"}),
        ("Tracing a Household through Census Records (Demo Exercise)", "A synthetic household illustrates how names, occupations, addresses, and enumeration errors can change between records.", {"period": "nineteenth century", "source_type": "synthetic census records", "analysis_focus": "record linkage"}),
        ("Interpreting a Factory Photograph (Demo History Guide)", "The note separates visible evidence from inference and asks who commissioned the image, who is absent, and what happened outside the frame.", {"period": "industrial era", "source_type": "photograph", "analysis_focus": "visual evidence"}),
        ("Newspaper Accounts of One Fictional Parade (Demo Exercise)", "Three invented newspaper excerpts use different headlines and details to demonstrate editorial framing and corroboration.", {"period": "early twentieth century", "source_type": "synthetic newspapers", "analysis_focus": "corroboration"}),
        ("Cataloging Community Posters (Demo Archive Note)", "A fictional poster collection is described by creator, date range, printing method, event type, and uncertainty rather than unsupported assumptions.", {"period": "late twentieth century", "source_type": "ephemera", "analysis_focus": "archival description"}),
        ("Following a Trade Route through Objects (Demo Exhibit Note)", "A synthetic exhibit groups ceramics, textiles, and tools to discuss exchange while clearly separating evidence from curatorial interpretation.", {"period": "pre-modern", "source_type": "material culture", "analysis_focus": "exchange networks"}),
        ("Evaluating a Wartime Diary (Demo History Guide)", "The guide discusses viewpoint, chronology, later editing, trauma, and comparison with independent sources without reproducing a real person's diary.", {"period": "twentieth century", "source_type": "fictional diary", "analysis_focus": "personal testimony"}),
        ("Revising a Museum Timeline (Demo Curation Note)", "A fictional museum team adds local voices, uncertainty labels, and multiple scales of time to a previously simplified display.", {"period": "multi-period", "source_type": "museum interpretation", "analysis_focus": "public history"}),
    ]
    return make_curated_collection(
        "history",
        ["archive", "maps", "oral history", "records", "photograph", "newspaper", "ephemera", "material culture", "diary", "museum"],
        rows,
        [field("period", "string"), field("source_type", "string"), field("analysis_focus", "string")],
    )


def make_travel() -> dict[str, object]:
    rows = [
        ("Porto Riverside Walk (Demo Itinerary)", "A non-current sample itinerary links a riverside walk, a market stop, and a museum visit with generous transit and rest time.", {"destination": "Porto, Portugal", "trip_style": "walking and culture", "duration_days": 2, "season_note": "check current local conditions before travel"}),
        ("Kyoto Neighborhood Loop (Demo Itinerary)", "A fictional day plan groups gardens, small museums, and rail connections by area to avoid unnecessary backtracking.", {"destination": "Kyoto, Japan", "trip_style": "rail and neighborhoods", "duration_days": 3, "season_note": "check current local conditions before travel"}),
        ("Chicago Architecture Weekend (Demo Itinerary)", "A sample weekend balances an architecture walk, public parks, indoor exhibits, and flexible meal stops.", {"destination": "Chicago, United States", "trip_style": "city and architecture", "duration_days": 2, "season_note": "check current weather and opening hours"}),
        ("Valparaíso Hills and Murals (Demo Itinerary)", "A sample route emphasizes daytime walking, public viewpoints, local transit planning, and respect for residential streets.", {"destination": "Valparaíso, Chile", "trip_style": "hills and public art", "duration_days": 2, "season_note": "verify current access and transit information"}),
        ("Copenhagen Design Route (Demo Itinerary)", "A fictional route connects design exhibits, waterfront public space, and bicycle-friendly corridors without claiming current availability.", {"destination": "Copenhagen, Denmark", "trip_style": "design and cycling", "duration_days": 3, "season_note": "verify current bicycle and venue information"}),
        ("Vancouver Rainy-Day Plan (Demo Itinerary)", "A flexible sample plan pairs indoor cultural stops with short outdoor walks and backup transit options.", {"destination": "Vancouver, Canada", "trip_style": "nature and museums", "duration_days": 2, "season_note": "check current weather and advisories"}),
        ("Marrakesh Courtyard Study Trip (Demo Itinerary)", "A fictional architecture-focused plan leaves time for guided context, rest, navigation, and respectful photography choices.", {"destination": "Marrakesh, Morocco", "trip_style": "architecture and markets", "duration_days": 3, "season_note": "verify current local guidance"}),
        ("Melbourne Tram and Gallery Days (Demo Itinerary)", "A sample city plan groups galleries, neighborhoods, and tram travel while keeping one unscheduled afternoon.", {"destination": "Melbourne, Australia", "trip_style": "transit and galleries", "duration_days": 4, "season_note": "check current transit and venue details"}),
        ("Ljubljana Slow City Break (Demo Itinerary)", "A fictional low-intensity itinerary uses walking, riverfront stops, and nearby green space with time for spontaneous changes.", {"destination": "Ljubljana, Slovenia", "trip_style": "slow city travel", "duration_days": 2, "season_note": "verify current local conditions"}),
        ("Oaxaca Food and Craft Notes (Demo Itinerary)", "A sample research itinerary centers on markets, workshops, museums, and asking permission before photographing people or work.", {"destination": "Oaxaca, Mexico", "trip_style": "food and craft", "duration_days": 3, "season_note": "verify current access and local guidance"}),
    ]
    result = make_curated_collection(
        "travel",
        ["itinerary", "culture", "city", "public transit", "museum", "architecture", "food", "walking"],
        rows,
        [field("destination", "string"), field("trip_style", "string"), field("duration_days", "int32"), field("season_note", "string")],
    )
    result["fixture_notice"] = SAFE_NOTICE + " Travel details are illustrative and not current travel advice."
    return result


def make_technology() -> dict[str, object]:
    rows = [
        ("Designing a Search API Contract (Demo Technical Note)", "Define stable request fields, explicit pagination, consistent errors, and versioning rules before optimizing implementation details.", {"topic": "API design", "audience": "backend engineers", "maturity": "design guide"}),
        ("Zero-Downtime Schema Migration (Demo Technical Note)", "A fictional rollout uses additive fields, dual reads, backfill checkpoints, metrics, and a reversible cutover.", {"topic": "database migration", "audience": "platform teams", "maturity": "operational pattern"}),
        ("Practical Service Observability (Demo Technical Note)", "Logs, metrics, and traces are tied to user-visible outcomes, with bounded cardinality and clear incident questions.", {"topic": "observability", "audience": "site reliability engineers", "maturity": "operational pattern"}),
        ("Threat Modeling a File Upload (Demo Technical Note)", "The note identifies trust boundaries, file validation, storage isolation, authorization, rate limits, and safe failure behavior.", {"topic": "application security", "audience": "security and product engineers", "maturity": "design exercise"}),
        ("Evaluating a Small Language Model Locally (Demo Technical Note)", "A synthetic evaluation set measures task quality, latency, memory, and failure modes without including private prompts or user data.", {"topic": "machine learning evaluation", "audience": "ML engineers", "maturity": "evaluation guide"}),
        ("Caching without Stale Surprises (Demo Technical Note)", "A fictional service documents cache keys, invalidation ownership, freshness limits, fallback behavior, and monitoring.", {"topic": "caching", "audience": "backend engineers", "maturity": "architecture guide"}),
        ("Accessible Mobile Navigation (Demo Technical Note)", "The design uses semantic labels, predictable focus order, large touch targets, reduced-motion support, and keyboard testing.", {"topic": "mobile accessibility", "audience": "mobile developers", "maturity": "implementation guide"}),
        ("Event Processing with Idempotency (Demo Technical Note)", "A synthetic order workflow uses stable event IDs, deduplication, retry limits, dead-letter review, and replay-safe handlers.", {"topic": "event-driven systems", "audience": "distributed-systems engineers", "maturity": "operational pattern"}),
        ("Robotics Sensor Fusion Basics (Demo Technical Note)", "A toy rover combines noisy distance and wheel measurements while exposing calibration assumptions and uncertainty.", {"topic": "robotics", "audience": "robotics students", "maturity": "instructional example"}),
        ("Reducing Cloud Cost with Evidence (Demo Technical Note)", "A fictional team profiles idle capacity, storage growth, and request patterns before testing small reversible changes.", {"topic": "cloud cost engineering", "audience": "platform teams", "maturity": "optimization guide"}),
    ]
    return make_curated_collection(
        "technology",
        ["software", "API", "migration", "observability", "security", "machine learning", "mobile", "events", "robotics", "cloud"],
        rows,
        [field("topic", "string"), field("audience", "string"), field("maturity", "string")],
    )


def make_sports() -> dict[str, object]:
    rows = [
        ("Harbor Foxes vs. Cedar Owls (Demo Match Report)", "In a fictional football match, the Harbor Foxes used wide overloads while the Cedar Owls protected central space and countered after turnovers.", {"sport": "football", "competition": "Example City League", "home_team": "Harbor Foxes", "away_team": "Cedar Owls", "result": "2-1"}),
        ("Northwind Comets vs. Valley Sparks (Demo Game Recap)", "A fictional basketball recap highlights defensive rebounding, bench minutes, and a late switch to zone coverage.", {"sport": "basketball", "competition": "Example Regional Cup", "home_team": "Northwind Comets", "away_team": "Valley Sparks", "result": "84-79"}),
        ("Blue River Aces vs. Summit Rackets (Demo Match Report)", "A fictional tennis tie turned on second-serve returns, patient baseline play, and careful workload management.", {"sport": "tennis", "competition": "Example Club Series", "home_team": "Blue River Aces", "away_team": "Summit Rackets", "result": "3-2"}),
        ("Prairie Runners Relay Review (Demo Event)", "A fictional relay team improved exchanges through shorter verbal cues, consistent check marks, and recovery planning.", {"sport": "track and field", "competition": "Example Open Meet", "home_team": "Prairie Runners", "away_team": "Lakeview Striders", "result": "fictional event"}),
        ("Granite Peaks vs. Maple Waves (Demo Match Report)", "A fictional volleyball report examines serve pressure, reception shape, middle-block timing, and substitution choices.", {"sport": "volleyball", "competition": "Example College League", "home_team": "Granite Peaks", "away_team": "Maple Waves", "result": "3-1"}),
        ("Orchard Blades vs. Copper Bears (Demo Game Recap)", "A fictional hockey game featured disciplined line changes, controlled zone entries, and strong penalty killing.", {"sport": "ice hockey", "competition": "Example Winter League", "home_team": "Orchard Blades", "away_team": "Copper Bears", "result": "4-3"}),
        ("Willow Creek Rowing Time Trial (Demo Event)", "A fictional rowing review compares stroke consistency, pacing, wind adjustment, and crew communication.", {"sport": "rowing", "competition": "Example River Regatta", "home_team": "Willow Creek Crew", "away_team": "Silver Oak Crew", "result": "fictional time trial"}),
        ("Desert Sky Cyclists Stage Review (Demo Event)", "A fictional cycling stage report focuses on hydration planning, crosswind positioning, mechanical checks, and shared team work.", {"sport": "cycling", "competition": "Example Desert Tour", "home_team": "Desert Sky Cyclists", "away_team": "Horizon Wheelers", "result": "fictional stage"}),
        ("Atlantic Harbor vs. Juniper Bay (Demo Match Report)", "A fictional baseball recap emphasizes pitch selection, defensive positioning, and the value of patient at-bats.", {"sport": "baseball", "competition": "Example Harbor Series", "home_team": "Atlantic Harbor", "away_team": "Juniper Bay", "result": "6-4"}),
        ("Clearbrook Swifts vs. Redwood Wings (Demo Match Report)", "A fictional badminton tie highlights net control, recovery steps, mixed-doubles communication, and rally construction.", {"sport": "badminton", "competition": "Example Community Cup", "home_team": "Clearbrook Swifts", "away_team": "Redwood Wings", "result": "4-1"}),
    ]
    return make_curated_collection(
        "sports",
        ["team", "match", "league", "tournament", "coach", "training", "score", "fictional sports"],
        rows,
        [field("sport", "string"), field("competition", "string"), field("home_team", "string"), field("away_team", "string"), field("result", "string")],
    )


def make_math() -> dict[str, object]:
    rows = [
        ("Solve 3x + 7 = 22 (Demo Problem)", "Subtract 7 from both sides and divide by 3, giving x = 5. The record is useful for equation and numeric-field search.", {"topic": "algebra", "expression": "3x + 7 = 22", "answer": "x = 5", "value": 5.0, "equation_index": 7}),
        ("Area of a 6 by 9 Rectangle (Demo Problem)", "Multiply length by width: 6 × 9 = 54 square units.", {"topic": "geometry", "expression": "6 * 9", "answer": "54 square units", "value": 54.0, "equation_index": 14}),
        ("Derivative of x Cubed (Demo Problem)", "Using the power rule, the derivative of x³ is 3x².", {"topic": "calculus", "expression": "d/dx x^3", "answer": "3x^2", "value": 3.0, "equation_index": 21}),
        ("Probability of Two Heads (Demo Problem)", "For two fair coin flips, one of four equally likely outcomes is two heads, so the probability is 0.25.", {"topic": "probability", "expression": "P(HH)", "answer": "1/4", "value": 0.25, "equation_index": 28}),
        ("Check Whether 101 Is Prime (Demo Problem)", "Test divisibility by primes no larger than √101; none divide evenly, so 101 is prime.", {"topic": "number theory", "expression": "prime(101)", "answer": "true", "value": 101.0, "equation_index": 35}),
        ("Add Two 2×2 Matrices (Demo Problem)", "Add corresponding entries of [[1,2],[3,4]] and [[4,3],[2,1]] to obtain [[5,5],[5,5]].", {"topic": "linear algebra", "expression": "[[1,2],[3,4]] + [[4,3],[2,1]]", "answer": "[[5,5],[5,5]]", "value": 20.0, "equation_index": 42}),
        ("Magnitude of Vector (3,4) (Demo Problem)", "The Euclidean magnitude is √(3² + 4²) = 5.", {"topic": "vectors", "expression": "sqrt(3^2 + 4^2)", "answer": "5", "value": 5.0, "equation_index": 49}),
        ("Mean of 4, 7, 9, and 12 (Demo Problem)", "The values sum to 32; dividing by four gives a mean of 8.", {"topic": "statistics", "expression": "(4 + 7 + 9 + 12) / 4", "answer": "8", "value": 8.0, "equation_index": 56}),
        ("Evaluate 2 to the Tenth Power (Demo Problem)", "Repeated doubling gives 2¹⁰ = 1024.", {"topic": "exponents", "expression": "2^10", "answer": "1024", "value": 1024.0, "equation_index": 63}),
        ("Integral of 2x from 0 to 3 (Demo Problem)", "An antiderivative is x²; evaluating from 0 to 3 gives 9.", {"topic": "integral calculus", "expression": "integral_0^3 2x dx", "answer": "9", "value": 9.0, "equation_index": 70}),
    ]
    result = make_curated_collection(
        "math",
        ["algebra", "geometry", "calculus", "probability", "prime", "matrix", "vector", "statistics", "equation", "integral"],
        rows,
        [field("topic", "string"), field("expression", "string"), field("answer", "string"), field("value", "float"), field("equation_index", "int32")],
    )
    result["default_sorting_field"] = "value"
    return result


def make_operational_collection(collection: str) -> dict[str, object]:
    profiles: dict[str, list[tuple[str, str]]] = {
        "saas": [
            ("subscription onboarding", "Maps invitation, first-run setup, activation signals, and respectful follow-up."),
            ("tenant administration", "Explains role assignment, audit history, safe defaults, and account boundaries."),
            ("usage analytics", "Defines transparent product metrics without storing unnecessary personal data."),
            ("billing recovery", "Uses clear notices, retry windows, and support escalation for failed demo payments."),
            ("workflow automation", "Documents triggers, idempotent actions, permissions, and operator overrides."),
            ("integration catalog", "Tracks API scopes, ownership, version support, and connection health."),
            ("dashboard design", "Prioritizes user decisions, accessible charts, and consistent metric definitions."),
            ("retention review", "Separates product engagement from coercive patterns and records test assumptions."),
            ("support operations", "Routes requests by urgency while preserving context and customer consent."),
            ("release management", "Uses staged rollout, monitoring, rollback criteria, and customer communication."),
        ],
        "finance": [
            ("household budget scenario", "Groups fictional income and expenses to demonstrate categories, filters, and monthly comparisons."),
            ("small-business cash-flow scenario", "Uses generated invoices and expenses to explain timing, reserves, and reconciliation."),
            ("loan affordability worksheet", "Demonstrates principal, rate, term, and sensitivity without recommending a real loan."),
            ("insurance comparison worksheet", "Compares fictional premiums, deductibles, exclusions, and service factors without advice."),
            ("payment operations review", "Tracks synthetic authorization, settlement, refund, and reconciliation events."),
            ("fraud-monitoring scenario", "Uses invented signals and explicit review steps without exposing real detection thresholds."),
            ("compliance checklist", "Demonstrates ownership, evidence, review dates, and exception tracking; it is not legal guidance."),
            ("portfolio learning example", "Uses fictional allocations to demonstrate sorting and arithmetic, not investment recommendations."),
            ("emergency-fund planner", "Shows generated savings scenarios and uncertainty without prescribing a personal target."),
            ("invoice aging report", "Uses synthetic receivables to demonstrate due-date ranges and follow-up workflows."),
        ],
        "fashion": [
            ("capsule wardrobe study", "A fictional collection tests color coordination, layering, repairability, and repeat wear."),
            ("material sourcing brief", "Compares generated fabric records by composition, traceability, durability, and care needs."),
            ("runway production plan", "Coordinates a fictional show schedule, fittings, lighting, accessibility, and backstage roles."),
            ("footwear design review", "Documents fit tests, outsole choices, repair considerations, and sample revisions."),
            ("streetwear lookbook", "Pairs fictional garments with art direction, sizing notes, and inclusive styling options."),
            ("accessories catalog", "Describes invented bags and jewelry by material, dimensions, care, and intended use."),
            ("seasonal color story", "Explains how a fictional palette moves across garments without making trend claims."),
            ("garment repair guide", "Maps common repairs, required tools, skill level, and when specialist help is appropriate."),
            ("inclusive sizing review", "Checks grading consistency, fit feedback, measurement language, and sample coverage."),
            ("fashion archive record", "Catalogs a fictional garment by construction, condition, provenance status, and storage needs."),
        ],
        "ecommerce": [
            ("product catalog quality", "Checks fictional titles, attributes, imagery notes, variants, and category consistency."),
            ("checkout reliability", "Maps cart, address, shipping, payment, confirmation, retry, and idempotency behavior."),
            ("inventory synchronization", "Tracks synthetic stock changes across a warehouse, storefront, and reservation queue."),
            ("shipping estimate design", "Explains transparent ranges, cutoff assumptions, and delay communication using demo orders."),
            ("returns workflow", "Documents eligibility, labels, inspection, refund status, and customer-visible updates."),
            ("marketplace seller review", "Uses fictional seller records to demonstrate verification, catalog quality, and support history."),
            ("search merchandising", "Combines relevance, availability, explicit promotions, and clear disclosure in a demo storefront."),
            ("conversion experiment", "Defines a synthetic hypothesis, guardrails, sample metrics, and stopping rules."),
            ("order support timeline", "Unifies fictional order events and customer contacts without exposing private data."),
            ("storefront accessibility", "Reviews keyboard navigation, labels, focus, errors, color contrast, and readable content."),
        ],
    }
    stages = ["discovery", "setup", "active use", "review", "improvement"]
    fields_by_collection = {
        "saas": [
            field("workflow", "string"),
            field("lifecycle_stage", "string"),
            field("customer_segment", "string"),
            field("status", "string"),
        ],
        "finance": [
            field("scenario", "string"),
            field("lifecycle_stage", "string"),
            field("risk_level", "string"),
            field("source", "string"),
        ],
        "fashion": [
            field("design_topic", "string"),
            field("lifecycle_stage", "string"),
            field("material_focus", "string"),
            field("season", "string"),
        ],
        "ecommerce": [
            field("commerce_topic", "string"),
            field("lifecycle_stage", "string"),
            field("channel", "string"),
            field("status", "string"),
        ],
    }
    documents: list[dict[str, object]] = []
    for index in range(50):
        topic, summary = profiles[collection][index % 10]
        stage = stages[index // 10]
        title = f"{topic.title()} — {stage.title()} (Demo Record {index + 1:02d})"
        content = (
            f"This synthetic {collection} record covers {topic} during the {stage} stage. "
            f"{summary} The scenario includes concrete fields and decisions so search results remain useful in a public demonstration."
        )
        extra: dict[str, object]
        if collection == "saas":
            extra = {"workflow": topic, "lifecycle_stage": stage, "customer_segment": ["small team", "mid-market", "enterprise demo"][index % 3], "status": ["planned", "active", "reviewing"][index % 3]}
        elif collection == "finance":
            extra = {"scenario": topic, "lifecycle_stage": stage, "risk_level": ["low demo", "medium demo", "high demo"][index % 3], "source": "synthetic_finance_scenario"}
        elif collection == "fashion":
            extra = {"design_topic": topic, "lifecycle_stage": stage, "material_focus": ["woven", "knit", "mixed material"][index % 3], "season": "fictional all-season collection"}
        else:
            extra = {"commerce_topic": topic, "lifecycle_stage": stage, "channel": ["web", "mobile", "marketplace demo"][index % 3], "status": ["planned", "active", "reviewing"][index % 3]}
        documents.append(
            document(
                collection,
                title,
                content,
                identifier=f"{collection}_{index + 1:03d}",
                labels=[topic, stage],
                data_notice=MARKET_NOTICE if collection == "finance" else SAFE_NOTICE,
                **extra,
            )
        )
    result = fixture(
        collection,
        [topic for topic, _ in profiles[collection]],
        documents,
        fields=fields_by_collection[collection],
    )
    if collection == "finance":
        result["fixture_notice"] = MARKET_NOTICE
    return result


def make_stocks() -> dict[str, object]:
    rows = [
        ("HLQ01", "Synthetic Broad Market Basket", "equity_index_demo", "balanced"),
        ("HLQ02", "Synthetic Technology Basket", "sector_index_demo", "growth"),
        ("HLQ03", "Synthetic Municipal Bond Basket", "bond_index_demo", "income"),
        ("HLQ04", "Synthetic Renewable Energy Basket", "sector_index_demo", "thematic"),
        ("HLQ05", "Synthetic Small Company Basket", "equity_index_demo", "small-cap"),
        ("HLQ06", "Synthetic Short-Term Treasury Basket", "bond_index_demo", "defensive"),
        ("HLQ07", "Synthetic Commodity Basket", "commodity_index_demo", "diversifier"),
        ("HLQ08", "Example Harbor Systems Equity", "fictional_equity", "software"),
        ("HLQ09", "Example Cedar Works Equity", "fictional_equity", "industrial"),
        ("HLQ10", "Example Blue River Foods Equity", "fictional_equity", "consumer"),
    ]
    documents = []
    for index, (ticker, name, asset_class, theme) in enumerate(rows):
        cashtag = f"${ticker}"
        content = (
            f"{name} ({ticker}, {cashtag}) is a completely fictional instrument used to demonstrate ticker, cashtag, "
            f"watchlist, and asset-class search. The synthetic scenario theme is {theme}; no price, performance, "
            "forecast, recommendation, or live market data is included."
        )
        documents.append(
            document(
                "stocks",
                f"{name} — Fictional Market Record",
                content,
                identifier=f"stocks_{ticker.lower()}",
                labels=[ticker, cashtag, asset_class, theme],
                data_notice=MARKET_NOTICE,
                ticker=ticker,
                cashtag=cashtag,
                asset_class=asset_class,
                watchlist=f"synthetic demo {theme}",
                source="HLQuery synthetic demo generator",
            )
        )
    result = fixture(
        "stocks",
        [row[0] for row in rows],
        documents,
        fields=[
            field("ticker", "string"),
            field("cashtag", "string"),
            field("asset_class", "string"),
            field("watchlist", "string"),
            field("source", "string"),
        ],
    )
    result["fixture_notice"] = MARKET_NOTICE
    return result


def make_anomalies() -> dict[str, object]:
    patterns = [
        ("latency_shift", "API latency moved above the synthetic baseline", "performance", "warning"),
        ("error_cluster", "Generated error events clustered within a short window", "reliability", "high"),
        ("login_burst", "Fictional login volume increased faster than the demo baseline", "security", "warning"),
        ("queue_backlog", "Synthetic queue depth grew while worker throughput stayed flat", "operations", "high"),
        ("refund_rate", "Generated refund events exceeded the fictional weekly pattern", "business", "warning"),
        ("cache_miss", "Synthetic cache misses rose after a demo configuration change", "performance", "info"),
        ("data_gap", "A generated telemetry series contains an unexpected interval", "data quality", "warning"),
        ("retry_loop", "A fictional client repeated the same idempotent request", "reliability", "high"),
        ("region_skew", "Synthetic request volume shifted toward one demo region", "operations", "info"),
        ("schema_drift", "A generated event field changed type in a demo producer", "data quality", "high"),
    ]
    services = ["search-api", "document-import", "analytics-worker", "auth-gateway", "billing-demo"]
    regions = ["demo-us-east", "demo-us-west", "demo-eu-central", "demo-ap-south"]
    documents = []
    for index in range(100):
        code, observation, category, severity = patterns[index % len(patterns)]
        service = services[(index * 3) % len(services)]
        region = regions[(index * 7) % len(regions)]
        baseline = f"synthetic baseline window {index % 5 + 1}"
        title = f"{code.replace('_', ' ').title()} in {service} (Demo Anomaly {index + 1:03d})"
        content = (
            f"{observation}. The affected fictional service is {service} in {region}. "
            f"Expected behavior follows {baseline}; the record exists to demonstrate anomaly search, severity filters, "
            "facets, and incident-review workflows without exposing a real system."
        )
        documents.append(
            document(
                "anomalies",
                title,
                content,
                identifier=f"anomalies_{index + 1:03d}",
                labels=[category, severity, service, region, code],
                category=category,
                summary=observation,
                source="synthetic_telemetry_generator",
                service=service,
                region=region,
                expected_pattern=baseline,
                observed_signal=observation,
                severity=severity,
                expected_label=code,
            )
        )
    return fixture(
        "anomalies",
        ["telemetry", "security", "business", "reliability", "performance", "data quality", "outlier", "baseline"],
        documents,
        fields=[
            field("category", "string"),
            field("summary", "string"),
            field("source", "string"),
            field("service", "string"),
            field("region", "string"),
            field("expected_pattern", "string"),
            field("observed_signal", "string"),
            field("severity", "string"),
            field("expected_label", "string"),
        ],
        stopwords=["the", "and"],
    )


def build_all() -> dict[str, dict[str, object]]:
    return {
        "anomalies.json": make_anomalies(),
        "art.json": make_art(),
        "books.json": make_books(),
        "ecommerce.json": make_operational_collection("ecommerce"),
        "fashion.json": make_operational_collection("fashion"),
        "finance.json": make_operational_collection("finance"),
        "food.json": make_food(),
        "history.json": make_history(),
        "math.json": make_math(),
        "movies.json": make_movies(),
        "music.json": make_music(),
        "people.json": make_people(),
        "saas.json": make_operational_collection("saas"),
        "science.json": make_science(),
        "sports.json": make_sports(),
        "stocks.json": make_stocks(),
        "technology.json": make_technology(),
        "travel.json": make_travel(),
        "universities.json": make_universities(),
    }


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    write(
        "_globals.json",
        {
            "fixture_version": 2,
            "fixture_notice": SAFE_NOTICE,
            "synonyms": [
                {
                    "id": "benchmark_global_demo",
                    "root": "demo",
                    "synonyms": ["sample", "fixture", "synthetic", "example"],
                },
                {
                    "id": "benchmark_global_guide",
                    "root": "guide",
                    "synonyms": ["tutorial", "walkthrough", "reference"],
                },
            ],
            "stopwords": ["benchmark", "collection", "document", "content", "inserted"],
        },
    )
    for filename, payload in build_all().items():
        write(filename, payload)


if __name__ == "__main__":
    main()
