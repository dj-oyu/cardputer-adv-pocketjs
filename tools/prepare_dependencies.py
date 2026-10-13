"""Explicitly initialize all required source submodules at their parent Gitlinks."""

import argparse

from prepare_submodules import PATHS, prepare_submodule


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kasane-only", action="store_true", help=argparse.SUPPRESS)
    parser.parse_args()
    for path in PATHS:
        prepare_submodule(path)


if __name__ == "__main__":
    main()
