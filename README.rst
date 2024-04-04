State
=====

Command-line tool for managing CM4all state directories.

State directories are a set of directories where the desired state of
software components is stored.


Building State
--------------

You need:

- a C++20 compliant compiler
- `Meson 0.56 <http://mesonbuild.com/>`__ and `Ninja <https://ninja-build.org/>`__
- `libfmt <https://fmt.dev/>`__

Get the source code::

 git clone --recursive https://github.com/CM4all/state

Run ``meson``::

 meson setup output

Compile and install::

 ninja -C output
 ninja -C output install
