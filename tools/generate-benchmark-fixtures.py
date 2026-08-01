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
    "Public HLQuery demonstration data. University names and campus locations "
    "are real catalog references; generated descriptions, people, organizations, "
    "artworks, rankings, incidents, and market instruments are synthetic."
)
MARKET_NOTICE = (
    "Synthetic demo scenario using no live market data; it is not investment, "
    "financial, legal, or trading advice."
)

PUBLIC_DEMO_METADATA = {
    "people": (
        "Fictional professional profiles for demonstrating name, biography, skills, occupation, and U.S. location search.",
        "science educator California | museum curator community exhibitions | civil engineer sustainable materials",
    ),
    "universities": (
        "One hundred recognizable U.S. universities with real names and campus locations plus clearly labeled synthetic benchmark signals.",
        "universities in Massachusetts | computer science universities | public research universities in Texas",
    ),
    "art": (
        "A fictional museum-style catalog covering painting, sculpture, printmaking, installation, drawing, and public art.",
        "recycled steel sculpture | charcoal portrait | installation about light and memory",
    ),
    "books": ("Fictional book records with useful genre, author, theme, and catalog metadata.", "climate fiction | oral history archive | mystery novel"),
    "music": ("Fictional releases and performances for artist, genre, instrument, and venue search.", "ambient electronic album | community jazz performance | string quartet"),
    "movies": ("Fictional film catalog records with genre, director, setting, and production context.", "documentary editing | science fiction drama | animated short"),
    "science": ("Instructional research examples that explain methods, controls, evidence, and uncertainty.", "battery discharge experiment | urban heat measurements | noisy sensor analysis"),
    "history": ("Public-history exercises about archives, maps, oral histories, photographs, and source interpretation.", "oral history consent | compare historical maps | museum timeline"),
    "food": ("Demonstration recipes with ingredients, cuisine context, method, and preparation time.", "lentil coconut recipe | vegetarian street food | citrus dessert"),
    "travel": ("Illustrative, non-current itineraries for destination, activity, duration, and travel-style search.", "architecture weekend | galleries and public transit | food and craft itinerary"),
    "technology": ("Practical fictional technical notes about APIs, reliability, accessibility, security, and data systems.", "search API design | zero downtime migration | accessible mobile navigation"),
    "sports": ("Fictional match and event reports with teams, tactics, competitions, and results.", "football counterattack | volleyball serve pressure | rowing pacing"),
    "math": ("Worked instructional examples spanning algebra, geometry, probability, calculus, and statistics.", "solve 3x plus 7 | probability two heads | derivative x cubed"),
    "ecommerce": ("Fictional commerce workflows covering catalogs, checkout, inventory, accessibility, and returns.", "checkout idempotency | inventory synchronization | accessible storefront"),
    "fashion": ("Fictional design and archive records covering materials, fit, production, repair, and inclusive sizing.", "garment repair | inclusive sizing | material sourcing"),
    "saas": ("Fictional software-service workflows covering onboarding, tenants, billing, support, and releases.", "tenant administration | release rollback | subscription onboarding"),
    "finance": ("Synthetic educational finance scenarios with no live data, recommendations, or personal records.", "invoice aging | household budget categories | payment reconciliation"),
    "stocks": ("Entirely fictional tickers and instruments for cashtag, watchlist, and asset-class search.", "$HLQ01 | fictional technology basket | synthetic bond instrument"),
    "anomalies": ("Synthetic operational signals for demonstrating severity, service, region, and incident search.", "queue backlog high severity | schema drift | latency shift"),
}


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
    description, demo_queries = PUBLIC_DEMO_METADATA.get(
        collection,
        (f"Synthetic {collection} records for public search demonstrations.", f"{collection} demo | synthetic {collection}"),
    )
    collection_metadata = {
        "_fixture_kind": "synthetic_public_demo",
        "_fixture_notice": SAFE_NOTICE,
        "_demo_description": description,
        "_demo_queries": demo_queries,
    }
    if metadata:
        collection_metadata.update(metadata)

    result: dict[str, object] = {
        "fixture_version": 3,
        "fixture_notice": SAFE_NOTICE,
        "collection": collection,
        "count": len(documents),
        "tags": tags,
        "documents": documents,
        "metadata": collection_metadata,
    }
    if fields:
        result["fields"] = fields
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
        (
            "River Grammar (Demo Print Series)",
            "Example Artist Amara Flint",
            "woodcut and water-based ink",
            "contemporary printmaking",
            2017,
            "Twelve woodcuts translate bends, currents, and floodplain marks into a visual vocabulary of repeated lines and open space.",
        ),
        (
            "Library of Unfinished Colors (Demo Installation)",
            "Example Artist Soren Field",
            "pigment, glass, and labeled shelves",
            "conceptual installation",
            2022,
            "Rows of fictional pigment samples invite visitors to compare how names, lighting, and neighboring colors change perception.",
        ),
        (
            "Three Chairs for a Long Conversation (Demo Sculpture)",
            "Example Artist Priya Moss",
            "carved ash and woven cord",
            "social sculpture",
            2020,
            "Three connected seats turn ordinary furniture into a study of distance, listening, and the physical design of conversation.",
        ),
        (
            "Night Bus Palimpsest (Demo Artwork)",
            "Example Artist Felix Arroyo",
            "gouache and graphite on paper",
            "narrative drawing",
            2019,
            "Erased figures, route numbers, and reflected windows accumulate into an imagined record of one late-night bus journey.",
        ),
        (
            "Seed Vault for Sidewalk Plants (Demo Archive)",
            "Example Artist Leila Brooks",
            "pressed plants, paper, and audio",
            "ecological archive art",
            2024,
            "A fictional neighborhood archive pairs resilient sidewalk plants with drawings and short recordings about observation and care.",
        ),
        (
            "Blue Room, Four Temperatures (Demo Painting)",
            "Example Artist Kenji Hale",
            "egg tempera on wood panel",
            "color field painting",
            2018,
            "Four panels use closely related blues to show how surface preparation and surrounding light can make one color feel warm or cool.",
        ),
        (
            "Public Steps, Private Rhythms (Demo Video Work)",
            "Example Artist Marisol Quill",
            "three-channel silent video",
            "observational media art",
            2021,
            "A staged sequence of fictional pedestrians focuses on repetition, pause, and the choreography created by shared civic space.",
        ),
        (
            "Repair Table No. 6 (Demo Textile Work)",
            "Example Artist Dae Winter",
            "mended cotton and linen",
            "textile art",
            2023,
            "Visible stitches preserve wear instead of hiding it, presenting repair as both a practical technique and a record of use.",
        ),
        (
            "Weather Station for Imagined Islands (Demo Sculpture)",
            "Example Artist Noor Bell",
            "brass, paper, and hand-built sensors",
            "speculative sculpture",
            2022,
            "Nonfunctional instruments describe the winds and rainfall of fictional islands while asking how measurement shapes a place.",
        ),
        (
            "Choir of Small Machines (Demo Sound Installation)",
            "Example Artist Tomas Grove",
            "motors, paper, wood, and speakers",
            "kinetic sound art",
            2024,
            "Low-speed motors animate paper surfaces to create a changing rhythm that visitors hear differently as they move through the room.",
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
        metadata={
            "_catalog_scope": "20 fictional artworks and installations",
            "_catalog_policy": "all artists, artworks, exhibitions, and provenance contexts are fictional",
        },
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
        ("community archivist", "neighborhood history and accessible archives", "oral history indexing | metadata design | public workshops", "community memory catalog"),
        ("software reliability engineer", "observability, incident learning, and developer mentoring", "distributed systems | tracing | incident facilitation", "service reliability field guide"),
        ("urban mobility planner", "safe walking routes and reliable public transit", "street design | transit analysis | community engagement", "safe routes accessibility map"),
        ("science educator", "hands-on astronomy and evidence-based classroom projects", "curriculum design | lab safety | science communication", "night-sky learning program"),
        ("museum programs curator", "community exhibitions and inclusive interpretation", "exhibition planning | accessibility | visitor research", "shared stories gallery program"),
        ("cooperative business adviser", "local entrepreneurship and practical budgeting", "cash-flow planning | facilitation | cooperative governance", "neighborhood enterprise workshop"),
        ("public health data analyst", "community wellness dashboards and data literacy", "data visualization | quality review | plain-language reporting", "community indicators dashboard"),
        ("civil infrastructure engineer", "resilient bridges and sustainable materials", "structural analysis | materials testing | project coordination", "low-carbon bridge materials study"),
        ("documentary editor", "visual storytelling and public-interest media", "story editing | archival research | accessible captions", "local voices short-film series"),
        ("food culture writer", "seasonal cooking and community recipe archives", "recipe testing | interviewing | cultural documentation", "seasonal table oral-history project"),
    ]
    career_stages = ["early-career", "mid-career", "senior", "lead", "independent"]
    project_phases = ["research", "community workshop", "prototype", "accessibility review", "public learning report"]
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
        role, interests, skills, project = roles[(index * 7) % len(roles)]
        city, state, latitude, longitude = locations[(index * 3 + index // 10 + 2) % len(locations)]
        organization = organizations[(index * 7 + index // 10) % len(organizations)]
        career_stage = career_stages[(index // 4) % len(career_stages)]
        experience_years = 2 + ((index * 7 + index // 10) % 19)
        featured_project = f"{project} — {project_phases[(index * 3 + index // 10) % len(project_phases)]}"
        full_name = f"{first} {middle} {last}"
        title = f"{full_name} — Fictional Demo Profile"
        biography = (
            f"{full_name} is a fictional {career_stage} {role} based in {city}, {state}. "
            f"This synthetic profile places them at {organization}, where the demo portfolio focuses on {interests}. "
            f"A featured project, {featured_project}, uses {skills}. The generated profile represents {experience_years} "
            "years of fictional experience and exists only to demonstrate biography, skills, occupation, project, and location search."
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
                skills=skills,
                featured_project=featured_project,
                career_stage=career_stage,
                experience_years=experience_years,
                profile_type="fictional_demo_person",
                location=[latitude, longitude],
                location_name=f"Fictional profile location near {city}, {state}",
            )
        )
    return fixture(
        "people",
        [role for role, _, _, _ in roles],
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
            field("skills", "string"),
            field("featured_project", "string"),
            field("career_stage", "string"),
            field("experience_years", "int32"),
            field("profile_type", "string"),
        ],
        metadata={
            "_profile_scope": "100 entirely fictional United States professional profiles",
            "_profile_policy": "no contact details, private records, or claims about real people",
        },
        synonyms=[
            {
                "id": "people_syn_biography",
                "root": "biography",
                "synonyms": ["profile", "background", "bio", "life summary"],
            }
        ],
    )


def make_universities() -> dict[str, object]:
    # Names, locations, and broad control/type labels are catalog data. Academic
    # topics and numeric scores below remain deterministic benchmark annotations.
    catalog = [
        ("Harvard University", "Cambridge", "Massachusetts", "private_research"),
        ("Stanford University", "Stanford", "California", "private_research"),
        ("Massachusetts Institute of Technology", "Cambridge", "Massachusetts", "private_research"),
        ("University of California Berkeley", "Berkeley", "California", "public_research"),
        ("University of Washington Seattle", "Seattle", "Washington", "public_research"),
        ("University of Michigan Ann Arbor", "Ann Arbor", "Michigan", "public_research"),
        ("Cornell University", "Ithaca", "New York", "private_research"),
        ("Columbia University", "New York", "New York", "private_research"),
        ("University of Pennsylvania", "Philadelphia", "Pennsylvania", "private_research"),
        ("Yale University", "New Haven", "Connecticut", "private_research"),
        ("Princeton University", "Princeton", "New Jersey", "private_research"),
        ("University of California Los Angeles", "Los Angeles", "California", "public_research"),
        ("University of Chicago", "Chicago", "Illinois", "private_research"),
        ("Johns Hopkins University", "Baltimore", "Maryland", "private_research"),
        ("University of California San Diego", "La Jolla", "California", "public_research"),
        ("University of Wisconsin Madison", "Madison", "Wisconsin", "public_research"),
        ("Duke University", "Durham", "North Carolina", "private_research"),
        ("Northwestern University", "Evanston", "Illinois", "private_research"),
        ("University of Illinois Urbana Champaign", "Urbana Champaign", "Illinois", "public_research"),
        ("New York University", "New York", "New York", "private_research"),
        ("University of Texas at Austin", "Austin", "Texas", "public_research"),
        ("University of North Carolina Chapel Hill", "Chapel Hill", "North Carolina", "public_research"),
        ("Pennsylvania State University", "University Park", "Pennsylvania", "public_research"),
        ("University of Minnesota Twin Cities", "Minneapolis", "Minnesota", "public_research"),
        ("University of Florida", "Gainesville", "Florida", "public_research"),
        ("University of Southern California", "Los Angeles", "California", "private_research"),
        ("Carnegie Mellon University", "Pittsburgh", "Pennsylvania", "private_research"),
        ("Georgia Institute of Technology", "Atlanta", "Georgia", "public_research"),
        ("Ohio State University", "Columbus", "Ohio", "public_research"),
        ("Purdue University", "West Lafayette", "Indiana", "public_research"),
        ("University of Maryland College Park", "College Park", "Maryland", "public_research"),
        ("University of California Davis", "Davis", "California", "public_research"),
        ("University of California Irvine", "Irvine", "California", "public_research"),
        ("University of California Santa Barbara", "Santa Barbara", "California", "public_research"),
        ("University of Colorado Boulder", "Boulder", "Colorado", "public_research"),
        ("University of Virginia", "Charlottesville", "Virginia", "public_research"),
        ("Vanderbilt University", "Nashville", "Tennessee", "private_research"),
        ("Rice University", "Houston", "Texas", "private_research"),
        ("Washington University in St Louis", "St Louis", "Missouri", "private_research"),
        ("Emory University", "Atlanta", "Georgia", "private_research"),
        ("University of Arizona", "Tucson", "Arizona", "public_research"),
        ("Arizona State University", "Tempe", "Arizona", "public_research"),
        ("Michigan State University", "East Lansing", "Michigan", "public_research"),
        ("Rutgers University New Brunswick", "New Brunswick", "New Jersey", "public_research"),
        ("Texas A and M University", "College Station", "Texas", "public_research"),
        ("Indiana University Bloomington", "Bloomington", "Indiana", "public_research"),
        ("University of Pittsburgh", "Pittsburgh", "Pennsylvania", "public_research"),
        ("Boston University", "Boston", "Massachusetts", "private_research"),
        ("Brown University", "Providence", "Rhode Island", "private_research"),
        ("Dartmouth College", "Hanover", "New Hampshire", "private_research"),
        ("University of Utah", "Salt Lake City", "Utah", "public_research"),
        ("University of Iowa", "Iowa City", "Iowa", "public_research"),
        ("Iowa State University", "Ames", "Iowa", "public_research"),
        ("University of Oregon", "Eugene", "Oregon", "public_research"),
        ("Oregon State University", "Corvallis", "Oregon", "public_research"),
        ("University of California Santa Cruz", "Santa Cruz", "California", "public_research"),
        ("University of California Riverside", "Riverside", "California", "public_research"),
        ("University of California San Francisco", "San Francisco", "California", "public_health_sciences"),
        ("University of Massachusetts Amherst", "Amherst", "Massachusetts", "public_research"),
        ("University of Connecticut", "Storrs", "Connecticut", "public_research"),
        ("University of Delaware", "Newark", "Delaware", "public_research"),
        ("University of Georgia", "Athens", "Georgia", "public_research"),
        ("University of Kansas", "Lawrence", "Kansas", "public_research"),
        ("University of Kentucky", "Lexington", "Kentucky", "public_research"),
        ("University of Missouri", "Columbia", "Missouri", "public_research"),
        ("University of Nebraska Lincoln", "Lincoln", "Nebraska", "public_research"),
        ("University of New Mexico", "Albuquerque", "New Mexico", "public_research"),
        ("University of Oklahoma", "Norman", "Oklahoma", "public_research"),
        ("University of South Carolina", "Columbia", "South Carolina", "public_research"),
        ("University of Tennessee Knoxville", "Knoxville", "Tennessee", "public_research"),
        ("University of Vermont", "Burlington", "Vermont", "public_research"),
        ("Virginia Tech", "Blacksburg", "Virginia", "public_research"),
        ("North Carolina State University", "Raleigh", "North Carolina", "public_research"),
        ("Florida State University", "Tallahassee", "Florida", "public_research"),
        ("University of Miami", "Coral Gables", "Florida", "private_research"),
        ("Georgetown University", "Washington", "District of Columbia", "private_research"),
        ("George Washington University", "Washington", "District of Columbia", "private_research"),
        ("Tufts University", "Medford", "Massachusetts", "private_research"),
        ("Northeastern University", "Boston", "Massachusetts", "private_research"),
        ("Syracuse University", "Syracuse", "New York", "private_research"),
        ("Rensselaer Polytechnic Institute", "Troy", "New York", "private_research"),
        ("University at Buffalo", "Buffalo", "New York", "public_research"),
        ("Stony Brook University", "Stony Brook", "New York", "public_research"),
        ("Binghamton University", "Binghamton", "New York", "public_research"),
        ("Temple University", "Philadelphia", "Pennsylvania", "public_research"),
        ("Drexel University", "Philadelphia", "Pennsylvania", "private_research"),
        ("Case Western Reserve University", "Cleveland", "Ohio", "private_research"),
        ("University of Cincinnati", "Cincinnati", "Ohio", "public_research"),
        ("University of Houston", "Houston", "Texas", "public_research"),
        ("Baylor University", "Waco", "Texas", "private_research"),
        ("Southern Methodist University", "Dallas", "Texas", "private_research"),
        ("Tulane University", "New Orleans", "Louisiana", "private_research"),
        ("Louisiana State University", "Baton Rouge", "Louisiana", "public_research"),
        ("University of Alabama", "Tuscaloosa", "Alabama", "public_research"),
        ("Auburn University", "Auburn", "Alabama", "public_research"),
        ("Clemson University", "Clemson", "South Carolina", "public_research"),
        ("Colorado State University", "Fort Collins", "Colorado", "public_research"),
        ("Washington State University", "Pullman", "Washington", "public_research"),
        ("University of Nevada Reno", "Reno", "Nevada", "public_research"),
        ("Brigham Young University", "Provo", "Utah", "private_research"),
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
    for catalog_order, (institution_name, city, state, institution_type) in enumerate(
        sorted(catalog, key=lambda row: row[0]), start=1
    ):
        index = catalog_order - 1
        focus_a, focus_b = focus_pairs[(index * 3 + index // 25) % len(focus_pairs)]
        title = institution_name
        search_topics = f"{focus_a} | {focus_b} | science | research | teaching | admissions"
        content = (
            f"{institution_name} is a real university catalog reference located in {city}, {state}, United States. "
            f"The catalog classifies it as {institution_type.replace('_', ' ')}. "
            f"Search topics attached for benchmark coverage include {focus_a}, {focus_b}, science, research, teaching, and admissions. "
            "The institution name, city, state, country, and broad type are catalog fields; topic annotations are synthetic search aids."
        )
        documents.append(
            document(
                "universities",
                title,
                content,
                identifier=f"universities_{slug(institution_name)}",
                labels=[state, city, "United States", institution_type, focus_a, focus_b, "university-catalog"],
                institution_name=institution_name,
                city=city,
                state=state,
                country="United States",
                city_aliases=f"{city} | {state} | United States",
                institution_type=institution_type,
                search_topics=search_topics,
                catalog_order=catalog_order,
                record_kind="real_name_location_with_synthetic_search_topics",
                location_name=f"{city}, {state}, United States",
            )
        )
    return fixture(
        "universities",
        ["united states", "university", "college", "campus", "science", "research", "teaching", "arts", "admissions"],
        documents,
        fields=[
            field("institution_name", "string"),
            field("state", "string"),
            field("city", "string"),
            field("country", "string"),
            field("city_aliases", "string"),
            field("institution_type", "string"),
            field("search_topics", "string"),
            field("catalog_order", "int32"),
            field("record_kind", "string"),
        ],
        default_sorting_field="catalog_order",
        metadata={
            "_catalog_sort_field": "catalog_order",
            "_catalog_sort_order": "asc",
            "_catalog_scope": "100 recognizable United States universities",
            "_data_boundary": "names, locations, and broad types are catalog references; search topics are synthetic",
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


def validate_public_fixtures(fixtures: dict[str, dict[str, object]]) -> None:
    """Reject public demo data that loses its safety markers or internal logic."""

    errors: list[str] = []
    all_ids: set[str] = set()

    for filename, payload in fixtures.items():
        collection = str(payload.get("collection", ""))
        documents = payload.get("documents", [])
        metadata = payload.get("metadata", {})

        if not collection or not isinstance(documents, list) or not documents:
            errors.append(f"{filename}: collection name and documents are required")
            continue
        if payload.get("fixture_version") != 3:
            errors.append(f"{filename}: fixture_version must be 3")
        if payload.get("count") != len(documents):
            errors.append(f"{filename}: count does not match documents")
        if not isinstance(metadata, dict) or metadata.get("_fixture_kind") != "synthetic_public_demo":
            errors.append(f"{filename}: public synthetic metadata is missing")
        if not metadata.get("_demo_description") or not metadata.get("_demo_queries"):
            errors.append(f"{filename}: demo description and example queries are required")

        collection_ids: set[str] = set()
        for index, item in enumerate(documents, start=1):
            prefix = f"{filename} document {index}"
            identifier = str(item.get("id", ""))
            labels = item.get("labels", [])
            if not identifier or identifier in collection_ids or identifier in all_ids:
                errors.append(f"{prefix}: id is missing or duplicated")
            collection_ids.add(identifier)
            all_ids.add(identifier)
            if item.get("is_synthetic") is not True:
                errors.append(f"{prefix}: is_synthetic must be true")
            if not isinstance(labels, list) or "demo" not in labels or "synthetic" not in labels:
                errors.append(f"{prefix}: demo and synthetic labels are required")
            if not str(item.get("data_notice", "")).strip():
                errors.append(f"{prefix}: data_notice is required")
            if not str(item.get("title", "")).strip() or not str(item.get("content", "")).strip():
                errors.append(f"{prefix}: meaningful title and content are required")

    people = fixtures["people.json"]["documents"]
    for index, item in enumerate(people, start=1):
        if "Fictional Demo Profile" not in str(item.get("title", "")):
            errors.append(f"people.json document {index}: title must identify a fictional demo profile")
        if item.get("profile_type") != "fictional_demo_person" or not str(item.get("organization", "")).startswith("Example "):
            errors.append(f"people.json document {index}: fictional person or organization marker is missing")
        if any(field_name in item for field_name in ("email", "phone", "address", "social_handle")):
            errors.append(f"people.json document {index}: public fixture must not include contact details")

    art = fixtures["art.json"]["documents"]
    for index, item in enumerate(art, start=1):
        if "Demo" not in str(item.get("title", "")) or not str(item.get("artist_name", "")).startswith("Example Artist "):
            errors.append(f"art.json document {index}: fictional artwork or artist marker is missing")

    universities = fixtures["universities.json"]["documents"]
    for expected_order, item in enumerate(universities, start=1):
        if item.get("catalog_order") != expected_order:
            errors.append(f"universities.json document {expected_order}: catalog order must be contiguous")
        if any(name in item for name in ("demo_rank", "rank", "composite_score", "student_count")):
            errors.append(f"universities.json document {expected_order}: fabricated rankings and counts are forbidden")
        if item.get("title") != item.get("institution_name") or not str(item.get("id", "")).startswith("universities_"):
            errors.append(f"universities.json document {expected_order}: meaningful catalog identity is missing")
        if str(item.get("id", "")).startswith("universities_demo_"):
            errors.append(f"universities.json document {expected_order}: generic numeric IDs are forbidden")

    for filename in ("finance.json", "stocks.json"):
        for index, item in enumerate(fixtures[filename]["documents"], start=1):
            if "not investment" not in str(item.get("data_notice", "")):
                errors.append(f"{filename} document {index}: financial disclaimer is missing")

    if errors:
        raise ValueError("Public benchmark fixture validation failed:\n- " + "\n- ".join(errors))


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    fixtures = build_all()
    validate_public_fixtures(fixtures)
    write(
        "_globals.json",
        {
            "fixture_version": 3,
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
    for filename, payload in fixtures.items():
        write(filename, payload)


if __name__ == "__main__":
    main()
