State
=====

State directories are a set of directories where the desired state of
software components is stored.

This provides a common interface to reconfigure certain runtime
aspects of different applications.  For example, it may be used to
disable services on certain cluster nodes and let the other nodes
provide these services instead.

Upon startup, the software reads files from these state directories.
Already-running processes can be asked to re-read these files by
sending the :ref:`RELOAD_STATE <reload_state>` control command.

The base directories are:

#. :file:`/run/cm4all/state` (transient runtime state; lost after
   reboot)
#. :file:`/etc/cm4all/state` (for state configured by the
   administrator)
#. :file:`/var/lib/cm4all/state` (for state managed by a software,
   e.g. some automatic cluster management system)
#. :file:`/lib/cm4all/state` (read-only defaults shipped with the
   operating system image)

The lookup of each setting happens in this order, i.e. :file:`/run`
overrides everything else, and :file:`/lib` is the last resort (if no
file exists in any of the directories, each software's hard-coded
defaults are the very-last resort, of course).  This ordering allows
both transient/temporary states and permanent states with a
well-defined override order, but without actually overwriting each
other's files destructively; once a higher-priority file gets deleted,
the system reverts to the lower-priority setting.

Each of those base directories should contain a top-level directory
for each application that loads state.  The directory structure within
these subdirectories is specific to that particular application.

Each setting is one file.  The following data types are available:

- *boolean*: ``1`` means true, ``0`` means false.

Symlinks
--------

Symlinks have a special meaning, and they are resolved in userspace,
not by the kernel, to implement this special meaning: they may point
to files in any of the base directories.  Example::

  /lib/cm4all/state/workshop/cron/foo/enabled -> ../../../pillar/foo/enabled
  /lib/cm4all/state/workshop/cron/foo/enabled -> /pillar/foo/enabled

Both symlinks targets are equal; an absolute target points to the root
of each base directory, not to the root filesystem.  They may point to
one of the following:

- :file:`/run/cm4all/state/pillar/foo/enabled`
- :file:`/etc/cm4all/state/pillar/foo/enabled`
- :file:`/var/lib/cm4all/state/pillar/foo/enabled`
- :file:`/lib/cm4all/state/pillar/foo/enabled`

This way, only a single file may need to be modified to change the
state of several daemons.  As usual, there may be overrides in the
other base directories.

Symlinks to directories are possible just as well.
