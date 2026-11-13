from pathlib import Path
from setuptools import setup, Distribution
from wheel.bdist_wheel import bdist_wheel

class Binary(Distribution):
    def has_ext_modules(self): return True

class Wheel(bdist_wheel):
    def finalize_options(self):
        super().finalize_options()
        self.root_is_pure = False
    def get_tag(self): return "py3", "none", "win_amd64"

if not (Path(__file__).parent / "interceptor/InterceptorBridge.dll").is_file():
    raise RuntimeError("Build the native bridge first with the product's native C ABI folder")
setup(distclass=Binary, cmdclass={"bdist_wheel": Wheel})
