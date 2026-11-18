"""Creates or updates /TickProfiler/TickProfilerConfiguration, the mod's SML Mod Configuration asset.

Run headless from the project root (the editor must not be open):
  UnrealEditor-Cmd.exe FactoryGame.uproject -run=pythonscript -script="<this file>" -unattended -nullrhi -nosplash

The asset is a Blueprint because SML's Mod Options widgets come from its Blueprint property classes
(the native UConfigProperty::CreateEditorWidget returns null). Keys, kinds and defaults must match
Source/TickProfiler/Private/TPSettings.cpp and the FSettings defaults in TPCollector.h.
"""
import unreal

MOD = "TickProfiler"
ASSET_DIR = f"/{MOD}"
ASSET_NAME = "TickProfilerConfiguration"
ASSET_PATH = f"{ASSET_DIR}/{ASSET_NAME}"
SML_PROPS = "/SML/Interface/UI/Menu/Mods/ConfigProperties"

COMMON_NOTE = (
    "These settings apply to the game they are set in. A dedicated server reads its own file,\n"
    "FactoryGame/Configs/TickProfiler.cfg; server admins can also change them with /perf set in chat."
)

# key, kind, default, display name, tooltip
SETTINGS = [
    ("Level", "int", 2, "Capture level (0-3)",
     "0 paused, 1 basic (frame stages), 2 detailed (+ which kinds of objects use the time),\n"
     "3 extreme (+ single objects; meant for short captures, e.g. /perf level 3 10)."),
    ("HitchMs", "float", 0.0, "Hitch threshold (ms, 0 = automatic)",
     "Frames that take longer than this are logged as hitches.\n0 = automatic: twice the target frame time, at least 50 ms."),
    ("SummaryIntervalSec", "int", 60, "Log summary every (seconds, 5-3600)",
     "How often a summary goes into FactoryGame.log."),
    ("ClassIntervalSec", "int", 10, "Class table every (seconds, 1-600)",
     "How often classes.csv gets a new set of rows."),
    ("SnapshotIntervalSec", "int", 300, "Object count every (seconds, 30-86400)",
     "How often snapshots.jsonl counts the objects in the world."),
    ("TopN", "int", 25, "Classes per table (1-500)",
     "How many classes each table lists."),
    ("MaxHitchLinesPerMinute", "int", 20, "Hitch lines per minute in the log (0-600)",
     "Limits hitch lines in FactoryGame.log. hitches.jsonl always gets every hitch."),
    ("DiskCapMB", "int", 256, "Disk space for data files (MB, at least 4)",
     "Total size of Logs/TickProfiler. The oldest data is deleted first. Applies at the next start."),
    ("WriteFiles", "bool", True, "Write data files",
     "Write the data files under Logs/TickProfiler. The log summaries are written either way. Applies at the next start."),
]

PROPERTY_CLASSES = {"int": "BP_ConfigPropertyInteger", "float": "BP_ConfigPropertyFloat", "bool": "BP_ConfigPropertyBool"}


def fail(message):
    unreal.log_error(f"[{MOD}] {message}")
    raise RuntimeError(message)


def load_sml_class(name):
    cls = unreal.load_class(None, f"{SML_PROPS}/{name}.{name}_C")
    if cls is None:
        fail(f"Could not load SML class {name}")
    return cls


def generated_class_of(blueprint):
    try:
        cls = unreal.BlueprintEditorLibrary.generated_class(blueprint)
        if cls is not None:
            return cls
    except Exception as error:  # library may be unavailable; fall back to loading by path
        unreal.log_warning(f"[{MOD}] BlueprintEditorLibrary.generated_class failed: {error}")
    return unreal.load_class(None, f"{ASSET_PATH}.{ASSET_NAME}_C")


def get_or_create_blueprint():
    """Updates the existing asset in place, so the asset never has to be deleted.

    Tries loading first: in the headless commandlet the asset registry has not scanned the plugin, so
    does_asset_exist() says False for an asset that exists, and create_asset() then collides with it.
    """
    blueprint = unreal.load_asset(ASSET_PATH)
    if blueprint is not None:
        return blueprint
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", unreal.ModConfiguration)
    blueprint = unreal.AssetToolsHelpers.get_asset_tools().create_asset(ASSET_NAME, ASSET_DIR, unreal.Blueprint, factory)
    if blueprint is None:
        fail("create_asset returned None")
    return blueprint


def get_or_create_subobject(existing, cls, outer, name):
    """Reuses an existing instanced subobject of the right class, so re-running never clashes on the object name."""
    if existing is not None and existing.get_class() == cls:
        return existing
    return unreal.new_object(cls, outer=outer, name=name)


def main():
    blueprint = get_or_create_blueprint()
    cls = generated_class_of(blueprint)
    if cls is None:
        fail("No generated class for the Blueprint")
    cdo = unreal.get_default_object(cls)

    section_cls = load_sml_class("BP_ConfigPropertySection")
    property_classes = {kind: load_sml_class(name) for kind, name in PROPERTY_CLASSES.items()}

    cdo.modify()
    root = get_or_create_subobject(cdo.get_editor_property("root_section"), section_cls, cdo, "RootSection")
    root.modify()
    existing_properties = root.get_editor_property("section_properties") or {}

    section_properties = {}
    for key, kind, default, display_name, tooltip in SETTINGS:
        prop = get_or_create_subobject(existing_properties.get(key), property_classes[kind], root, key)
        prop.modify()
        prop.set_editor_property("display_name", display_name)
        prop.set_editor_property("tooltip", f"{tooltip}\n{COMMON_NOTE}")
        prop.set_editor_property("default_value", default)
        prop.set_editor_property("value", default)
        prop.set_editor_property("requires_world_reload", False)
        section_properties[key] = prop
    root.set_editor_property("section_properties", section_properties)
    # SML defaults sections to a horizontal list (with a scroll bar); the Mods menu reads top to bottom, so stack them.
    root.set_editor_property("widget_type", unreal.CP_SectionWidgetType.CPS_VERTICAL)

    config_id = unreal.ConfigId()
    config_id.set_editor_property("mod_reference", MOD)
    config_id.set_editor_property("config_category", "")
    cdo.set_editor_property("config_id", config_id)
    cdo.set_editor_property("display_name", "Tick Profiler")
    cdo.set_editor_property("description", "How much Tick Profiler records and how much disk it may use.")
    cdo.set_editor_property("root_section", root)

    if not unreal.EditorAssetLibrary.save_loaded_asset(blueprint, False):
        fail("Saving the asset failed")
    unreal.log(f"[{MOD}] Saved {ASSET_PATH}")


main()
