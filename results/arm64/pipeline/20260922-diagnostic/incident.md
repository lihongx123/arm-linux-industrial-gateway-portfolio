# Diagnostic launch failed before QEMU startup

The first diagnostic runner attempted shutil.copy2 from WSL ext4 to the Windows
evidence directory. The data copy completed, but preserving file timestamps
failed with PermissionError in copystat/utime. No guest ran and no performance
result exists. The runner now uses a unique WSL /tmp runtime image and copyfile.
The partial runtime.ext4 is an infrastructure artifact, excluded from publication.
