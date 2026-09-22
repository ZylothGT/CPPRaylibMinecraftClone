from pathlib import Path
from PIL import Image
import json
import math
import shutil


# ------------------------------------------------------------
# Paths
# ------------------------------------------------------------

ROOT = Path(__file__).resolve().parent.parent

INPUT_DIR = ROOT / "assets" / "blocks"
OUTPUT_DIR = ROOT / "generated"
BLOCK_OUTPUT_DIR = OUTPUT_DIR / "blocks"

ATLAS_FILE = "blocks_atlas.png"
ATLAS_JSON = "atlas.json"

# Pixels around every texture for mipmap padding
PADDING = 8

# Empty tiles around the outside of the atlas
ATLAS_BORDER_TILES = 0


# ------------------------------------------------------------
# JSON helper
# ------------------------------------------------------------

def write_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)

    with path.open("w", encoding="utf-8") as file:
        json.dump(data, file, indent=4)
        file.write("\n")


# ------------------------------------------------------------
# UV helper
# ------------------------------------------------------------

def make_uv(x, y, width, height, atlas_width, atlas_height):
    return {
        "u0": x / atlas_width,
        "v0": y / atlas_height,
        "u1": (x + width) / atlas_width,
        "v1": (y + height) / atlas_height
    }

def add_padding(image, padding):
    width, height = image.size

    padded = Image.new(
        "RGBA",
        (
            width + padding * 2,
            height + padding * 2
        )
    )

    # Center texture
    padded.paste(
        image,
        (padding, padding)
    )

    # Top
    top = image.crop((0, 0, width, 1))
    padded.paste(
        top.resize((width, padding)),
        (padding, 0)
    )

    # Bottom
    bottom = image.crop((0, height - 1, width, height))
    padded.paste(
        bottom.resize((width, padding)),
        (padding, height + padding)
    )

    # Left
    left = image.crop((0, 0, 1, height))
    padded.paste(
        left.resize((padding, height)),
        (0, padding)
    )

    # Right
    right = image.crop((width - 1, 0, width, height))
    padded.paste(
        right.resize((padding, height)),
        (width + padding, padding)
    )

    # Corners
    padded.paste(
        image.crop((0, 0, 1, 1)).resize((padding, padding)),
        (0, 0)
    )

    padded.paste(
        image.crop((width - 1, 0, width, 1)).resize((padding, padding)),
        (width + padding, 0)
    )

    padded.paste(
        image.crop((0, height - 1, 1, height)).resize((padding, padding)),
        (0, height + padding)
    )

    padded.paste(
        image.crop((width - 1, height - 1, width, height))
        .resize((padding, padding)),
        (width + padding, height + padding)
    )

    return padded


# ------------------------------------------------------------
# Find blocks
# ------------------------------------------------------------

def find_blocks():
    if not INPUT_DIR.exists():
        raise RuntimeError(
            f"Could not find input directory:\n{INPUT_DIR}"
        )

    blocks = []

    for directory in sorted(INPUT_DIR.iterdir(),
                            key=lambda p: p.name.lower()):

        if not directory.is_dir():
            continue

        textures = {}

        for name in ("all", "top", "bottom", "side"):
            path = directory / f"{name}.png"

            if path.exists():
                textures[name] = path

        if not textures:
            print(
                f"WARNING: '{directory.name}' has no textures, skipping."
            )
            continue

        blocks.append((directory.name, textures))

    if not blocks:
        raise RuntimeError(
            f"No block folders were found in:\n{INPUT_DIR}"
        )

    return blocks


# ------------------------------------------------------------
# Load textures
# ------------------------------------------------------------

def load_textures(blocks):
    loaded = {}

    expected_size = None

    for block_name, textures in blocks:
        loaded[block_name] = {}

        for texture_name, path in textures.items():

            try:
                image = Image.open(path).convert("RGBA")
            except Exception as error:
                raise RuntimeError(
                    f"Could not load texture '{path}': {error}"
                )

            if expected_size is None:
                expected_size = image.size

            elif image.size != expected_size:
                raise RuntimeError(
                    "All block textures must have the same size.\n\n"
                    f"Expected: {expected_size[0]}x{expected_size[1]}\n"
                    f"Found:    {image.width}x{image.height}\n"
                    f"File:     {path}"
                )

            loaded[block_name][texture_name] = image

    return loaded, expected_size


# ------------------------------------------------------------
# Resolve block faces
# ------------------------------------------------------------

def resolve_faces(block_name, textures):
    all_texture = textures.get("all")

    # If all.png exists, use it as the fallback.
    if all_texture is not None:
        return {
            "top": textures.get("top", all_texture),
            "bottom": textures.get("bottom", all_texture),
            "north": textures.get("side", all_texture),
            "south": textures.get("side", all_texture),
            "east": textures.get("side", all_texture),
            "west": textures.get("side", all_texture)
        }

    # Otherwise top/bottom/side are required.
    required = ("top", "bottom", "side")

    for name in required:
        if name not in textures:
            raise RuntimeError(
                f"Block '{block_name}' does not have all.png "
                f"and is missing '{name}.png'"
            )

    return {
        "top": textures["top"],
        "bottom": textures["bottom"],
        "north": textures["side"],
        "south": textures["side"],
        "east": textures["side"],
        "west": textures["side"]
    }


# ------------------------------------------------------------
# Main generator
# ------------------------------------------------------------

def generate():

    print("================================")
    print("Voxel Texture Atlas Generator")
    print("================================")
    print()

    print(f"Input : {INPUT_DIR}")
    print(f"Output: {OUTPUT_DIR}")
    print()

    blocks = find_blocks()

    loaded, texture_size = load_textures(blocks)

    texture_width, texture_height = texture_size

    # --------------------------------------------------------
    # Find unique textures
    # --------------------------------------------------------

    unique_textures = {}
    block_faces = {}

    for block_name, _ in blocks:

        faces = resolve_faces(
            block_name,
            loaded[block_name]
        )

        block_faces[block_name] = {}

        for face, image in faces.items():

            # Using image bytes means identical images only
            # appear once in the atlas.
            key = image.tobytes()

            if key not in unique_textures:
                unique_textures[key] = image

            block_faces[block_name][face] = key

    texture_count = len(unique_textures)

    # --------------------------------------------------------
    # Calculate atlas dimensions
    # --------------------------------------------------------

    columns = max(
        1,
        math.ceil(math.sqrt(texture_count))
    )

    rows = math.ceil(
        texture_count / columns
    )

    slot_width = texture_width + PADDING * 2
    slot_height = texture_height + PADDING * 2

    atlas_width = (
        columns + ATLAS_BORDER_TILES * 2
    ) * slot_width

    atlas_height = (
        rows + ATLAS_BORDER_TILES * 2
    ) * slot_height

    atlas = Image.new(
        "RGBA",
        (atlas_width, atlas_height),
        (0, 0, 0, 0)
    )

    # --------------------------------------------------------
    # Put textures into atlas
    # --------------------------------------------------------

    locations = {}

    for index, (key, image) in enumerate(
        unique_textures.items()
    ):

        column = index % columns
        row = index // columns

        slot_x = (
            column + ATLAS_BORDER_TILES
        ) * slot_width

        slot_y = (
            row + ATLAS_BORDER_TILES
        ) * slot_height

        x = slot_x + PADDING
        y = slot_y + PADDING

        padded_image = add_padding(image, PADDING)

        atlas.paste(
            padded_image,
            (slot_x, slot_y)
        )

        locations[key] = {
            "x": x,
            "y": y,
            "width": texture_width,
            "height": texture_height,
            "column": column,
            "row": row
        }

    # --------------------------------------------------------
    # Prepare output directories
    # --------------------------------------------------------

    OUTPUT_DIR.mkdir(
        parents=True,
        exist_ok=True
    )

    if BLOCK_OUTPUT_DIR.exists():
        shutil.rmtree(BLOCK_OUTPUT_DIR)

    BLOCK_OUTPUT_DIR.mkdir(
        parents=True,
        exist_ok=True
    )

    # --------------------------------------------------------
    # Save atlas
    # --------------------------------------------------------

    atlas_path = OUTPUT_DIR / ATLAS_FILE

    atlas.save(atlas_path)

    # --------------------------------------------------------
    # Generate atlas.json
    # --------------------------------------------------------

    atlas_json = {
        "version": 1,

        "atlas": {
            "file": ATLAS_FILE,
            "width": atlas_width,
            "height": atlas_height,
            "tileWidth": texture_width,
            "tileHeight": texture_height,
            "columns": columns,
            "rows": rows,
            "borderTiles": ATLAS_BORDER_TILES
        },

        "textures": {}
    }

    key_to_texture_name = {}

    for index, key in enumerate(unique_textures):

        texture_name = f"texture_{index}"

        key_to_texture_name[key] = texture_name

        location = locations[key]

        atlas_json["textures"][texture_name] = {
            "pixel": {
                "x": location["x"],
                "y": location["y"],
                "width": location["width"],
                "height": location["height"]
            },

            "uv": make_uv(
                location["x"],
                location["y"],
                location["width"],
                location["height"],
                atlas_width,
                atlas_height
            )
        }

    write_json(
        OUTPUT_DIR / ATLAS_JSON,
        atlas_json
    )

    # --------------------------------------------------------
    # Generate one JSON for every block
    # --------------------------------------------------------

    for block_name, _ in blocks:

        block_json = {
            "name": block_name,
            "textures": {}
        }

        for face, key in block_faces[block_name].items():

            location = locations[key]

            block_json["textures"][face] = {
                "atlasTexture": key_to_texture_name[key],

                "pixel": {
                    "x": location["x"],
                    "y": location["y"],
                    "width": location["width"],
                    "height": location["height"]
                },

                "uv": make_uv(
                    location["x"],
                    location["y"],
                    location["width"],
                    location["height"],
                    atlas_width,
                    atlas_height
                )
            }

        write_json(
            BLOCK_OUTPUT_DIR / f"{block_name}.json",
            block_json
        )

    # --------------------------------------------------------
    # Print summary
    # --------------------------------------------------------

    print()
    print("Generated successfully!")
    print()
    print(f"Blocks:          {len(blocks)}")
    print(f"Unique textures: {texture_count}")
    print(
        f"Texture size:    "
        f"{texture_width}x{texture_height}"
    )
    print(
        f"Atlas size:      "
        f"{atlas_width}x{atlas_height}"
    )
    print()
    print(f"Atlas:            {atlas_path}")
    print(
        f"Atlas metadata:   "
        f"{OUTPUT_DIR / ATLAS_JSON}"
    )
    print(
        f"Block metadata:   "
        f"{BLOCK_OUTPUT_DIR}"
    )
    print()


# ------------------------------------------------------------
# Entry point
# ------------------------------------------------------------

if __name__ == "__main__":
    try:
        generate()
    except Exception as error:
        print()
        print(f"ERROR: {error}")
        print()
        raise SystemExit(1)