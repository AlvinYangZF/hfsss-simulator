# Kbuild for hfsss_nvme kernel module
obj-m += hfsss_nvme.o
hfsss_nvme-objs := src/kernel/hfsss_nvme_kmod.o \
                   src/kernel/hfsss_nvme_pci.o  \
                   src/kernel/hfsss_nvme_queue.o \
                   src/kernel/hfsss_nvme_shmem.o

ccflags-y := -Iinclude -Iinclude/kernel -Iinclude/pcie
