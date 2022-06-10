import shlex
from setuptools import setup, Command

class CoverageCommand(Command):
    user_options = []

    def initialize_options(self):
        pass

    def finalize_options(self):
        pass

    def run(self):
        import sys, subprocess
        subprocess.run(shlex.split("coverage run --source hyperon -m unittest discover"), check=True,
                       cwd='./tests')
        subprocess.run(shlex.split("coverage html"), check=True)
        raise SystemExit(0)


setup(name='hyperon',
      version='0.1',
      description='Hyperon API in Python',
      packages=['hyperon'],
      install_requires=[],
      extras_require={
          'dev': [
              'coverage'
          ]
      },
      cmdclass={
          'coverage': CoverageCommand,
          }
     )
