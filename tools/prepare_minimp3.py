"""Initialize minimp3 at the parent Gitlink without following an upstream branch."""

from prepare_submodules import prepare_submodule


def prepare() -> None:
    prepare_submodule("third_party/minimp3")


if __name__ == "__main__":
    prepare()
