"""
Build the native core extension.

    python bmm_chess/build_core.py

Run it as a script, not `python -m bmm_chess.build_core`: -m imports the
package first, and the package refuses to import without this extension.
"""

import os
import shutil
import sys


def build(quiet: bool = False) -> str:
    from setuptools import Distribution, Extension
    from setuptools.command.build_ext import build_ext

    here = os.path.dirname(os.path.abspath(__file__))
    source = os.path.join(here, "_core.c")
    if not os.path.exists(source):
        raise FileNotFoundError(source)

    if os.name == "nt":
        extra = ["/O2"]
    else:
        extra = ["-O3", "-fomit-frame-pointer"]

    ext = Extension(
        "bmm_chess._core",
        sources=[source],
        extra_compile_args=extra,
    )

    dist = Distribution({"name": "bmm_chess", "ext_modules": [ext]})
    dist.script_args = ["build_ext"]
    cmd = build_ext(dist)
    cmd.inplace = 0
    cmd.build_lib = os.path.join(here, "_build", "lib")
    cmd.build_temp = os.path.join(here, "_build", "temp")
    cmd.force = 1
    cmd.verbose = not quiet
    cmd.ensure_finalized()
    cmd.run()

    built = cmd.get_ext_fullpath("bmm_chess._core")
    target = os.path.join(here, os.path.basename(built))

    # Windows will not overwrite a .pyd another process still has mapped.
    if os.path.exists(target):
        try:
            os.remove(target)
        except OSError:
            stale = target + ".old"
            try:
                if os.path.exists(stale):
                    os.remove(stale)
            except OSError:
                pass
            try:
                os.replace(target, stale)
            except OSError as exc:
                raise RuntimeError(
                    f"cannot replace {target}: still loaded by another process"
                ) from exc

    shutil.copyfile(built, target)
    return target


def main() -> int:
    target = build()
    print("built %s" % target)
    return 0


if __name__ == "__main__":
    sys.exit(main())
