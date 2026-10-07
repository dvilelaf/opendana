/* Dependency category of every public test source. Every translation unit
 * has an explicit category; a missing one fails, it never defaults to unit.
 * The public tree holds only `unit` sources: synthetic or model-only, no
 * imported content and no reference data. */
{"test_catalog.c", "*", "unit", "synthetic custom levels and own temporary files"},
{"test_clock.c", "*", "unit", "integer and rational clock model"},
{"test_enemy_unit.c", "*", "unit", "own synthetic collision grids and entities, semantic type API"},
{"test_hud_unit.c", "*", "unit", "synthetic clock states, latch and text formatting"},
{"test_import.c", "*", "unit", "standard SHA-256 vectors and a synthetic invalid file"},
{"test_input_map.c", "*", "unit", "synthetic masks, axes, bindings and own settings"},
{"test_leveled.c", "*", "unit", "synthetic editor model, JSON and validation"},
{"test_objects_unit.c", "*", "unit", "semantic object classes, pipe masks and unsupported types"},
{"test_save.c", "*", "unit", "synthetic progression save format and own files"},
{"test_session.c", "*", "unit", "atomic save, invalid copy, backup and per-user dirs on own temporary files"},
{"test_settings.c", "*", "unit", "synthetic settings and gain buffer"},
{"test_ui_catalog.c", "*", "unit", "own custom levels and modern UI geometry"},
{"test_ui_editor.c", "*", "unit", "own synthetic editor document and preview"},
{"test_ui_strings.c", "*", "unit", "modern interface strings"},
{"test_video.c", "*", "unit", "viewport arithmetic"},
{"test_worldmap.c", "*", "unit", "synthetic progression, markers and destinations"},
{"test_worldmap_hidden.c", "*", "unit", "synthetic title input, hidden menu input and progression bitmap"},
