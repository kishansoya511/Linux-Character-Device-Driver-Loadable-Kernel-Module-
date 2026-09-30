# Linux-Character-Device-Driver-Loadable-Kernel-Module-
A Linux character device driver (LKM) with dynamic major/minor allocation, cdev-based registration, kmalloc'd buffer, mutex-protected concurrent access, and full file_operations (open/read/write/llseek/release).
# mychardev - Simple Linux Character Device Driver

A loadable kernel module implementing a character device with:
- Dynamic major/minor allocation (`alloc_chrdev_region`)
- `cdev`-based registration
- Automatic `/dev` node creation (`class_create` + `device_create`)
- `kmalloc`/`kfree` dynamic kernel buffer
- Mutex-protected shared buffer
- Full `file_operations`: `open`, `read`, `write`, `release`, `llseek`

## Requirements

- Linux with kernel headers installed for your running kernel:
  ```bash
  # Debian/Ubuntu
  sudo apt install build-essential linux-headers-$(uname -r)

  # Fedora/RHEL
  sudo dnf install kernel-devel-$(uname -r) gcc make
  ```
- **Recommended**: test inside a VM, not your host machine — kernel bugs
  can crash or hang the entire OS, not just one process.

## Build the driver

```bash
cd driver
make
```
Produces `mychardev.ko` (plus intermediate build artifacts).

## Load the driver

```bash
sudo insmod mychardev.ko
dmesg | tail -n 5                      # confirm "module loaded successfully"
cat /proc/devices | grep mychardev     # confirm major number registered
ls -l /dev/mychardev                   # confirm device node exists
lsmod | grep mychardev                 # confirm module + usage count
```

## Build and run the test application

```bash
cd ../test
gcc -o test_app test_app.c
sudo ./test_app
dmesg | tail -n 10                     # cross-check kernel-side logs
```

Expected `test_app` output:
```
[test_app] Device opened successfully, fd = 3
[test_app] Wrote 23 bytes: "Hello from user space!"
[test_app] Seeked back to position 0
[test_app] Read 23 bytes: "Hello from user space!"
[test_app] SEEK_END position: 1024
[test_app] SEEK_CUR (-5) position: 1019
[test_app] Device closed successfully
```

## Unload the driver

```bash
sudo rmmod mychardev
dmesg | tail -n 5                      # confirm "module unloaded successfully"
ls /dev/mychardev                      # should now report "No such file or directory"
```

## Clean build artifacts

```bash
cd driver
make clean
```

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| `insmod: Invalid module format` | Module built against a different kernel (`vermagic` mismatch) | `make clean && make`, ensure `/lib/modules/$(uname -r)/build` matches running kernel |
| `insmod: Operation not permitted` | Missing `sudo`, or Secure Boot blocking unsigned modules | Use `sudo`; check `mokutil --sb-state` |
| `open()` fails: "No such file or directory" | Module not loaded, or `device_create()` failed | Check `lsmod`, `dmesg` for errors |
| `open()` fails: "Permission denied" | Device node is root-only (`crw-------`) | Run test app with `sudo` |
| `rmmod` fails: "Module is in use" | Something still has `/dev/mychardev` open | `lsof /dev/mychardev`, close it, retry |
| `read()`/`write()` hangs | Mutex deadlock (missing unlock on some path) | Check `dmesg` for matching enter/exit log pairs |

## Project structure

```
chardriver_project/
├── driver/
│   ├── Makefile        # Kbuild wrapper -- builds mychardev.ko
│   └── mychardev.c     # The kernel module source
├── test/
│   └── test_app.c      # User-space test program
```
Author
Kisha Soya
