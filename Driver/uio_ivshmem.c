#include <linux/device.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/uio_driver.h>

#define PCI_DEVICE_ID_QEMU_IVSHMEM  0x1110

#define IVSHMEM_REGISTER_MASK       0
#define IVSHMEM_REGISTER_STATUS     4

static irqreturn_t handle_interrupt(int interruption, struct uio_info* info)
{
  struct pci_dev* device;
  void __iomem* status;

  device = (struct pci_dev*)info->priv;
  status = info->mem[0].internal_addr + IVSHMEM_REGISTER_STATUS;

  if ((!device->msix_enabled) &&
      (!readl(status)))
  {
    // Peer is a revision 0 without MSI-X support
    return IRQ_NONE;
  }

  return IRQ_HANDLED;
}

static int probe(struct pci_dev* device, const struct pci_device_id* identifier)
{
  struct uio_info* info;

  if ((info = (struct uio_info*)kzalloc(sizeof(struct uio_info), GFP_KERNEL)))
  {
    if (pci_enable_device(device) == 0)
    {
      if (pci_request_regions(device, "ivshmem") == 0)
      {
        if ((info->mem[0].addr          = pci_resource_start(device, 0)) &&
            (info->mem[0].internal_addr = pci_ioremap_bar(device, 0)))
        {
          if (pci_alloc_irq_vectors(device, 1, 1, PCI_IRQ_LEGACY | PCI_IRQ_MSIX) >= 1)
          {
            info->mem[0].size    = pci_resource_len(device, 0);
            info->mem[0].size    = (info->mem[0].size + PAGE_SIZE - 1) & PAGE_SIZE;
            info->mem[0].memtype = UIO_MEM_PHYS;
            info->mem[0].name    = "control";
            info->mem[1].addr    = pci_resource_start(device, 2);
            info->mem[1].size    = pci_resource_len(device, 2);
            info->mem[1].memtype = UIO_MEM_PHYS;
            info->mem[1].name    = "memory";
            info->irq            = pci_irq_vector(device, 0);
            info->irq_flags      = IRQF_SHARED;
            info->handler        = handle_interrupt;
            info->name           = "uio_ivshmem";
            info->version        = "0.1";
            info->priv           = device;

            pci_set_master(device);

            if (uio_register_device(&device->dev, info) == 0)
            {
              if (!device->msix_enabled)
              {
                // Peer is a revision 0 without MSI-X support
                writel(1, info->mem[0].internal_addr + IVSHMEM_REGISTER_MASK);
              }

              pci_set_drvdata(device, info);
              return 0;
            }

            pci_clear_master(device);
            pci_free_irq_vectors(device);
          }
          iounmap(info->mem[0].internal_addr);
        }
        pci_release_regions(device);
      }
      pci_disable_device(device);
    }
    kfree(info);
    return -ENODEV;
  }

  return -ENOMEM;
}

static void remove(struct pci_dev* device)
{
  struct uio_info* info;

  info = (struct uio_info*)pci_get_drvdata(device);

  pci_set_drvdata(device, NULL);
  uio_unregister_device(info);
  pci_clear_master(device);
  pci_free_irq_vectors(device);
  iounmap(info->mem[0].internal_addr);
  pci_release_regions(device);
  pci_disable_device(device);
  kfree(info);
}

static struct pci_device_id ivshmem_identifiers[] = 
{
  {
    .vendor    = PCI_VENDOR_ID_REDHAT_QUMRANET,
    .device    = PCI_DEVICE_ID_QEMU_IVSHMEM,
    .subvendor = PCI_ANY_ID,
    .subdevice = PCI_ANY_ID,
  },
  { 
    0,
  }
};

static struct pci_driver ivshmem_driver =
{
  .name     = "uio_ivshmem",
  .id_table = ivshmem_identifiers,
  .probe    = probe,
  .remove   = remove,
};

module_pci_driver(ivshmem_driver);
MODULE_DEVICE_TABLE(pci, ivshmem_identifiers);
MODULE_LICENSE("GPL v2");
