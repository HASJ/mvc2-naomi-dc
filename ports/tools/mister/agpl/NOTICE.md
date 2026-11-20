# MiSTer wrapper

This folder is a derived work, under the GNU Affero General Public License 3.0
(LICENSE), kept apart from the rest of the port.

- `wrapper/` builds MiSTer_MVC2 from MiSTer-devel/Main_MiSTer (GPL 3) at
  3380931329b8acb442bd3d35a24d89f88641b7cf. `main-mister.patch` carries 3s-mister-arm's changes to
  fpga_io, input and user_io (child shutdown on core load, pad export, OSD trigger capture) and
  drops the OSD's core browser. `mvc2_wrapper.cpp` is a cut-down version of 3s-mister-arm's
  thirdsarm_wrapper.cpp; `mvc2_joy_shm.h` is its mister_joy_shm.h renamed.
