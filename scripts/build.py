#!/usr/bin/env python3

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Optional

# Switch to project root directory.
os.chdir(Path(__file__).resolve().parent.parent)

################################################################################
# Single-target project constants
################################################################################

_BOARD = "generic/esp32-s3-camera-robot"
_BOARD_DIR = Path("main/boards") / _BOARD
_BOARD_CONFIG_SYMBOL = "CONFIG_BOARD_TYPE_ESP32_S3_CAMERA_ROBOT"
_SUPPORTED_TARGET = "esp32s3"
_SUPPORTED_LANGUAGES = ("en-US", "vi-VN")

_WAKE_WORD_MODEL_PATTERN = re.compile(r"^wn9[sl]?_[a-z0-9_]+$")
_ESP_SR_KCONFIG = Path("managed_components/espressif__esp-sr/Kconfig.projbuild")
_REPORTED_IDENTIFIER_PATTERN = re.compile(r"^[a-z0-9.-]+$")


################################################################################
# ESP-IDF helpers
################################################################################


def get_project_version() -> Optional[str]:
    """Read set(PROJECT_VER "x.y.z") from the root CMakeLists.txt."""
    with Path("CMakeLists.txt").open(encoding="utf-8") as file:
        for line in file:
            if line.startswith("set(PROJECT_VER"):
                return line.split('"')[1]
    return None


def _get_idf_command() -> list[str]:
    """Return the active ESP-IDF idf.py command."""
    idf_path = os.environ.get("IDF_PATH")
    if idf_path:
        idf_py = Path(idf_path) / "tools" / "idf.py"
        if idf_py.is_file():
            return [sys.executable, str(idf_py)]

    raise RuntimeError(
        "ESP-IDF environment is not initialized correctly. "
        "IDF_PATH/tools/idf.py was not found."
    )


def _run_idf(*args: str, preview: bool = False) -> None:
    command = _get_idf_command()
    if preview:
        command.append("--preview")
    command.extend(args)
    if subprocess.run(command, check=False).returncode != 0:
        print(f"{' '.join(command)} failed", file=sys.stderr)
        sys.exit(1)


def merge_bin(preview: bool = False) -> None:
    _run_idf("merge-bin", preview=preview)


################################################################################
# Board configuration
################################################################################


def _validate_reported_identifier(value: object, field: str) -> str:
    """Validate compatibility-sensitive OTA-reported identifiers."""
    if not isinstance(value, str) or not value:
        raise ValueError(f"missing non-empty {field}")
    if not _REPORTED_IDENTIFIER_PATTERN.fullmatch(value):
        raise ValueError(
            f"{field} {value!r} must contain only lowercase letters, "
            'digits, "." and "-"'
        )
    return value


def _load_board_config(
    config_filename: str = "config.json",
) -> tuple[dict, dict]:
    """Load the one supported desk-robot board configuration."""
    cfg_path = _BOARD_DIR / config_filename
    if not cfg_path.is_file():
        raise RuntimeError(f"Board configuration not found: {cfg_path}")

    with cfg_path.open(encoding="utf-8") as file:
        cfg = json.load(file)

    target = cfg.get("target")
    if target != _SUPPORTED_TARGET:
        raise ValueError(
            f"{cfg_path}: target must be {_SUPPORTED_TARGET!r}, got {target!r}"
        )

    _validate_reported_identifier(cfg.get("type"), 'top-level "type"')

    builds = cfg.get("builds")
    if not isinstance(builds, list) or len(builds) != 1:
        raise ValueError(
            f"{cfg_path}: standalone desk-robot config must contain exactly "
            "one build entry"
        )

    build = builds[0]
    if not isinstance(build, dict):
        raise ValueError(f"{cfg_path}: build entry must be an object")

    _validate_reported_identifier(build.get("name"), 'build "name"')

    sdkconfig_append = build.get("sdkconfig_append", [])
    if not isinstance(sdkconfig_append, list) or not all(
        isinstance(item, str) for item in sdkconfig_append
    ):
        raise ValueError(f"{cfg_path}: sdkconfig_append must be a string list")

    preview = cfg.get("preview", False)
    if not isinstance(preview, bool):
        raise ValueError(f"{cfg_path}: preview must be a boolean")

    return cfg, build


def _board_summary(config_filename: str = "config.json") -> dict[str, object]:
    cfg, build = _load_board_config(config_filename)
    manufacturer = cfg.get("manufacturer")
    name = _validate_reported_identifier(build.get("name"), 'build "name"')
    full_name = (
        f"{manufacturer}-{name}"
        if isinstance(manufacturer, str)
        and manufacturer
        and not name.startswith(f"{manufacturer}-")
        else name
    )
    return {
        "board": _BOARD,
        "name": name,
        "full_name": full_name,
        "type": _validate_reported_identifier(
            cfg.get("type"), 'top-level "type"'
        ),
        "target": _SUPPORTED_TARGET,
        "config": _BOARD_CONFIG_SYMBOL,
        "wake_word_supported": True,
    }


################################################################################
# sdkconfig helpers
################################################################################


def _sdkconfig_assignments(options: list[str]) -> dict[str, str]:
    """Return the final value for each CONFIG_* assignment."""
    assignments: dict[str, str] = {}
    for option in options:
        key, separator, value = option.strip().partition("=")
        if not separator or not key.startswith("CONFIG_"):
            raise ValueError(f"Invalid sdkconfig assignment: {option!r}")
        assignments[key] = value
    return assignments


def _merge_sdkconfig_options(
    base_options: list[str],
    override_options: list[str],
) -> list[str]:
    """Merge sdkconfig assignments by key, with later overrides winning."""
    keys: list[str] = []
    values: dict[str, str] = {}

    for option in (*base_options, *override_options):
        key = option.split("=", 1)[0]
        if key not in values:
            keys.append(key)
        values[key] = option

    return [values[key] for key in keys]


def _language_sdkconfig_option(language: str) -> tuple[str, str]:
    """Normalize en-US / vi-VN and return its Kconfig assignment."""
    normalized_input = language.strip().replace("_", "-").casefold()
    by_casefold = {
        supported.casefold(): supported
        for supported in _SUPPORTED_LANGUAGES
    }

    try:
        normalized = by_casefold[normalized_input]
    except KeyError as error:
        supported = ", ".join(_SUPPORTED_LANGUAGES)
        raise ValueError(
            f"Unsupported language {language!r}. Supported values: {supported}"
        ) from error

    symbol = normalized.replace("-", "_").upper()
    return normalized, f"CONFIG_LANGUAGE_{symbol}=y"


################################################################################
# Wake word helpers
################################################################################


def _collect_wake_words(
    kconfig_path: Path = _ESP_SR_KCONFIG,
) -> list[dict[str, object]]:
    """Read available WakeNet models from the resolved ESP-SR component."""
    if not kconfig_path.exists():
        raise RuntimeError(
            f"{kconfig_path} was not found. Resolve managed components with "
            "'idf.py reconfigure' before listing wake words."
        )

    content = kconfig_path.read_text(encoding="utf-8")
    config_matches = list(
        re.finditer(
            r"^\s*config SR_WN_([A-Z0-9_]+)\s*$",
            content,
            re.MULTILINE,
        )
    )

    wake_words: list[dict[str, object]] = []
    for index, match in enumerate(config_matches):
        block_end = (
            config_matches[index + 1].start()
            if index + 1 < len(config_matches)
            else len(content)
        )
        block = content[match.end():block_end]
        label_match = re.search(
            r'^\s*bool "([^"]+)"\s*$',
            block,
            re.MULTILINE,
        )
        if not label_match:
            continue

        model = match.group(1).lower()
        label = label_match.group(1)
        suffix = f"({model})"
        phrase = (
            label[:-len(suffix)].strip()
            if label.endswith(suffix)
            else label
        )
        wake_words.append(
            {
                "model": model,
                "phrase": phrase,
                "targets": [_SUPPORTED_TARGET],
            }
        )

    if not wake_words:
        raise RuntimeError(f"No WakeNet models found in {kconfig_path}")

    return wake_words


def _print_wake_word_list(
    wake_words: list[dict[str, object]],
) -> None:
    model_width = max(
        len("MODEL"),
        *(len(str(item["model"])) for item in wake_words),
    )
    phrase_width = max(
        len("PHRASE"),
        *(len(str(item["phrase"])) for item in wake_words),
    )

    print(f"{'MODEL':<{model_width}}  {'PHRASE':<{phrase_width}}  TARGETS")
    for item in wake_words:
        targets = ",".join(str(target) for target in item["targets"])
        print(
            f"{str(item['model']):<{model_width}}  "
            f"{str(item['phrase']):<{phrase_width}}  {targets}"
        )
    print("\nSpecial values: nihaoxiaozhi, disabled")


def _enabled_default_wake_word_symbols() -> list[str]:
    """Return WakeNet models enabled by project / ESP32-S3 defaults."""
    symbols: list[str] = []

    for path in (
        Path("sdkconfig.defaults"),
        Path("sdkconfig.defaults.esp32s3"),
    ):
        if not path.exists():
            continue

        for line in path.read_text(encoding="utf-8").splitlines():
            match = re.fullmatch(
                r"(CONFIG_SR_WN_[A-Z0-9_]+)=y",
                line.strip(),
            )
            if match and match.group(1) not in symbols:
                symbols.append(match.group(1))

    return symbols


def _wake_word_sdkconfig_options(
    wake_word: str,
) -> tuple[str, list[str], list[str]]:
    """Map a WakeNet model to project/ESP-SR Kconfig options."""
    normalized = wake_word.strip().casefold().replace("-", "_")
    if normalized == "nihaoxiaozhi":
        normalized = "wn9_nihaoxiaozhi_tts"

    options = [
        f"{symbol}=n"
        for symbol in _enabled_default_wake_word_symbols()
    ]
    options.extend(
        [
            "CONFIG_WAKE_WORD_DISABLED=n",
            "CONFIG_USE_AFE_WAKE_WORD=n",
            "CONFIG_USE_CUSTOM_WAKE_WORD=n",
        ]
    )

    if normalized == "disabled":
        options.append("CONFIG_WAKE_WORD_DISABLED=y")
        return normalized, options, ["CONFIG_WAKE_WORD_DISABLED"]

    if not _WAKE_WORD_MODEL_PATTERN.fullmatch(normalized):
        raise ValueError(
            f"Invalid wake word {wake_word!r}. Use 'disabled', "
            "'nihaoxiaozhi', or an ESP-SR model name such as "
            "'wn9_jarvis_tts'."
        )

    implementation = "CONFIG_USE_AFE_WAKE_WORD"
    model_symbol = f"CONFIG_SR_WN_{normalized.upper()}"
    options.extend(
        [
            f"{implementation}=y",
            f"{model_symbol}=y",
        ]
    )
    return normalized, options, [implementation, model_symbol]


################################################################################
# Target / configure helpers
################################################################################


def _target_from_sdkconfig() -> Optional[str]:
    sdkconfig = Path("sdkconfig")
    if not sdkconfig.exists():
        return None

    match = re.search(
        r'^CONFIG_IDF_TARGET="([^"]+)"$',
        sdkconfig.read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    return match.group(1) if match else None


def _target_from_cmake_cache() -> Optional[str]:
    cache = Path("build/CMakeCache.txt")
    if not cache.exists():
        return None

    match = re.search(
        r"^IDF_TARGET(?::[^=]+)?=(.+)$",
        cache.read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    return match.group(1).strip() if match else None


def _configured_target() -> Optional[str]:
    """Return the current target when sdkconfig/CMake state is consistent."""
    sdkconfig_target = _target_from_sdkconfig()
    cache_target = _target_from_cmake_cache()

    if (
        sdkconfig_target
        and cache_target
        and sdkconfig_target != cache_target
    ):
        return None

    return cache_target or sdkconfig_target


def _sync_vscode_target(
    target: str,
    settings_path: Path = Path(".vscode/settings.json"),
) -> bool:
    """Keep an existing VS Code ESP-IDF target setting in sync."""
    if not settings_path.exists():
        return False

    content = settings_path.read_text(encoding="utf-8")
    pattern = re.compile(
        r'(?P<prefix>"IDF_TARGET"\s*:\s*")'
        r'(?:\\.|[^"\\])*'
        r'(?P<suffix>")'
    )
    updated, matches = pattern.subn(
        lambda match: (
            f'{match.group("prefix")}{target}{match.group("suffix")}'
        ),
        content,
    )

    if matches == 0 or updated == content:
        return False

    settings_path.write_text(updated, encoding="utf-8")
    print(f"[INFO] Updated {settings_path} IDF_TARGET to {target}.")
    return True


def _prepare_target(target: str, preview: bool) -> None:
    """Full-clean only when an existing CMake build targets another chip."""
    cache_target = _target_from_cmake_cache()
    current_target = _configured_target()

    if current_target == target:
        print(f"[INFO] Reusing target {target}.")
        return

    if cache_target and cache_target != target:
        print(
            f"[INFO] Switching target from {cache_target} to {target}."
        )
        _run_idf("fullclean", preview=preview)
    elif current_target:
        print(
            f"[INFO] Configuring target {target} "
            f"(was {current_target})."
        )
    else:
        print(f"[INFO] Configuring target {target}.")


def _configure_build(
    sdkconfig_append: list[str],
    board_name: str,
    preview: bool,
) -> None:
    """Configure the fixed ESP32-S3 target and selected user options."""
    sdkconfig = Path("sdkconfig")
    sdkconfig_old = Path("sdkconfig.old")

    if sdkconfig.exists():
        if sdkconfig_old.exists():
            sdkconfig_old.unlink()
        sdkconfig.replace(sdkconfig_old)

    fragment = Path("build/xiaozhi-build.sdkconfig.defaults")
    fragment.parent.mkdir(parents=True, exist_ok=True)
    fragment.write_text(
        "# Generated by scripts/build.py\n"
        + "\n".join(sdkconfig_append)
        + "\n",
        encoding="utf-8",
    )

    defaults: list[str] = []
    if Path("sdkconfig.defaults").exists():
        defaults.append("sdkconfig.defaults")
    defaults.append(fragment.as_posix())

    _run_idf(
        f"-DIDF_TARGET={_SUPPORTED_TARGET}",
        f"-DSDKCONFIG_DEFAULTS={';'.join(defaults)}",
        f"-DBOARD_NAME={board_name}",
        "reconfigure",
        preview=preview,
    )
    _sync_vscode_target(_SUPPORTED_TARGET)


def _validate_configured_symbols(
    symbols: list[str],
    option_name: str,
) -> None:
    """Ensure Kconfig accepted user-selected options."""
    if not symbols:
        return

    sdkconfig = Path("sdkconfig")
    if not sdkconfig.exists():
        raise RuntimeError(
            f"Cannot validate {option_name}: sdkconfig was not generated"
        )

    configured = sdkconfig.read_text(encoding="utf-8")
    missing = [
        symbol
        for symbol in symbols
        if not re.search(
            rf"^{re.escape(symbol)}=y$",
            configured,
            re.MULTILINE,
        )
    ]
    if missing:
        raise ValueError(
            f"{option_name} is incompatible with the current project "
            f"configuration; Kconfig rejected: {', '.join(missing)}"
        )


################################################################################
# Build
################################################################################


def build_robot(
    *,
    config_filename: str = "config.json",
    name_filter: Optional[str] = None,
    language: Optional[str] = None,
    wake_word: Optional[str] = None,
) -> None:
    """Configure and build the single supported ESP32-S3 desk robot."""
    cfg, build = _load_board_config(config_filename)

    reported_type = _validate_reported_identifier(
        cfg.get("type"),
        'top-level "type"',
    )
    board_name = _validate_reported_identifier(
        build.get("name"),
        'build "name"',
    )

    if name_filter is not None and name_filter != board_name:
        raise ValueError(
            f"Only build name {board_name!r} is supported; "
            f"got {name_filter!r}"
        )

    preview = bool(cfg.get("preview", False))
    sdkconfig_append = list(build.get("sdkconfig_append", []))

    # The standalone repository has one physical target and one board symbol.
    sdkconfig_append = _merge_sdkconfig_options(
        sdkconfig_append,
        [f"{_BOARD_CONFIG_SYMBOL}=y"],
    )

    selected_language: Optional[str] = None
    selected_wake_word: Optional[str] = None

    validations: list[tuple[list[str], str]] = [
        ([_BOARD_CONFIG_SYMBOL], "board selection"),
    ]
    user_options: list[str] = []

    if language is not None:
        selected_language, option = _language_sdkconfig_option(language)
        user_options.append(option)
        validations.append(
            ([option.split("=", 1)[0]], "--language")
        )

    if wake_word is not None:
        (
            selected_wake_word,
            wake_options,
            wake_symbols,
        ) = _wake_word_sdkconfig_options(wake_word)
        user_options.extend(wake_options)
        validations.append((wake_symbols, "--wake-word"))

    sdkconfig_append = _merge_sdkconfig_options(
        sdkconfig_append,
        user_options,
    )
    _sdkconfig_assignments(sdkconfig_append)

    print("-" * 80)
    print(f"project_version: {get_project_version()}")
    print(f"board: {_BOARD}")
    print(f"name: {board_name}")
    print(f"reported_type: {reported_type}")
    print(f"target: {_SUPPORTED_TARGET}")
    if selected_language:
        print(f"language: {selected_language}")
    if selected_wake_word:
        print(f"wake_word: {selected_wake_word}")
    for item in sdkconfig_append:
        print(f"sdkconfig_append: {item}")

    os.environ.pop("IDF_TARGET", None)
    _prepare_target(_SUPPORTED_TARGET, preview)
    _configure_build(
        sdkconfig_append,
        board_name,
        preview,
    )

    for symbols, option_name in validations:
        _validate_configured_symbols(symbols, option_name)

    _run_idf("build", preview=preview)
    merge_bin(preview)


################################################################################
# CLI
################################################################################


def _validate_board_argument(board: Optional[str]) -> None:
    """Keep old invocation syntax without retaining multi-board machinery."""
    if board is None:
        return
    if board in (_BOARD, "all"):
        return

    raise ValueError(
        f"This standalone repository supports only {_BOARD!r}; "
        f"got {board!r}"
    )


def main(argv: Optional[list[str]] = None) -> None:
    parser = argparse.ArgumentParser(
        description="Configure and build the Xiaozhi ESP32-S3 desk robot.",
    )
    parser.add_argument(
        "board",
        nargs="?",
        default=None,
        help=(
            f"Optional compatibility argument; only {_BOARD!r} is supported"
        ),
    )
    parser.add_argument(
        "-c",
        "--config",
        default="config.json",
        help="Board config filename (default: config.json)",
    )
    parser.add_argument(
        "--list-boards",
        action="store_true",
        help="Print the single supported board",
    )
    parser.add_argument(
        "--list-languages",
        action="store_true",
        help="List values accepted by --language",
    )
    parser.add_argument(
        "--list-wake-words",
        action="store_true",
        help="List WakeNet models from the resolved ESP-SR component",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Use JSON output with a list command",
    )
    parser.add_argument(
        "--name",
        help="Compatibility option; must match the single configured build name",
    )
    parser.add_argument(
        "--language",
        metavar="LOCALE",
        help="Firmware language: en-US or vi-VN",
    )
    parser.add_argument(
        "--wake-word",
        metavar="MODEL",
        help=(
            "Wake-word model (for example wn9_jarvis_tts), "
            "'nihaoxiaozhi', or 'disabled'"
        ),
    )

    cli_args = sys.argv[1:] if argv is None else argv
    if not cli_args:
        parser.print_help()
        return

    args = parser.parse_args(cli_args)

    list_count = sum(
        (
            bool(args.list_boards),
            bool(args.list_languages),
            bool(args.list_wake_words),
        )
    )
    if list_count > 1:
        parser.error("only one --list-* option may be used at a time")

    if args.list_languages:
        if any(
            (
                args.board,
                args.name,
                args.language,
                args.wake_word,
            )
        ):
            parser.error(
                "--list-languages cannot be combined with build options"
            )
        values = list(_SUPPORTED_LANGUAGES)
        print(
            json.dumps(values)
            if args.json
            else "\n".join(values)
        )
        return

    if args.list_wake_words:
        if any(
            (
                args.board,
                args.name,
                args.language,
                args.wake_word,
            )
        ):
            parser.error(
                "--list-wake-words cannot be combined with build options"
            )
        try:
            wake_words = _collect_wake_words()
        except RuntimeError as error:
            print(f"[ERROR] {error}", file=sys.stderr)
            sys.exit(1)

        if args.json:
            print(json.dumps(wake_words, ensure_ascii=False))
        else:
            _print_wake_word_list(wake_words)
        return

    if args.list_boards:
        if any(
            (
                args.board,
                args.name,
                args.language,
                args.wake_word,
            )
        ):
            parser.error(
                "--list-boards cannot be combined with build options"
            )
        board = _board_summary(args.config)
        if args.json:
            print(json.dumps([board]))
        else:
            print(board["board"])
            print(f"  - {board['name']}")
        return

    if args.json:
        parser.error("--json is only valid with a --list-* option")

    try:
        _validate_board_argument(args.board)
        build_robot(
            config_filename=args.config,
            name_filter=args.name,
            language=args.language,
            wake_word=args.wake_word,
        )
    except (RuntimeError, ValueError) as error:
        print(f"[ERROR] {error}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
