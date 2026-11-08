Import("env")
import os

APP_BIN = os.path.join("$BUILD_DIR", "${PROGNAME}.bin")

def app_partition_size(env):
    # The size of the app partition in the env's partition table, e.g. 0x170000 for the 4 MB clocks
    # and 0x200000 for the 8 MB ones. upload.maximum_size is the whole flash, not the app slot.
    partitions = env.GetProjectOption("board_build.partitions")
    path = os.path.join(env["PROJECT_DIR"], partitions)
    with open(path) as f:
        for line in f:
            fields = [field.strip() for field in line.split(",")]
            if len(fields) >= 5 and fields[1] == "app":
                return int(fields[4], 0)
    raise Exception("No app partition in " + path)

def check_bin_size(source, target, env):
    # PlatformIO's own size check counts sections, but the .bin is several K bigger because of
    # segment headers and padding. If it doesn't fit, uploading it overwrites the start of the
    # file system and the bootloader refuses to start it, so the clock boot loops.
    bin_path = target[0].get_abspath()
    size = os.path.getsize(bin_path)
    max_size = app_partition_size(env)

    if size > max_size:
        os.remove(bin_path)    # So it can't be uploaded
        print("Error: %s is %d bytes, %d bytes bigger than the %d byte app partition" % (bin_path, size, size - max_size, max_size))
        return 1

    print("%s is %d bytes, %d bytes free in the app partition" % (os.path.basename(bin_path), size, max_size - size))

env.AddPostAction(APP_BIN, check_bin_size)
