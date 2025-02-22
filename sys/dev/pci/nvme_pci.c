/*	$NetBSD: nvme_pci.c,v 1.36 2022/07/09 01:24:32 pgoyette Exp $	*/
/*	$OpenBSD: nvme_pci.c,v 1.3 2016/04/14 11:18:32 dlg Exp $ */

/*
 * Copyright (c) 2014 David Gwynne <dlg@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*-
 * Copyright (C) 2016 NONAKA Kimihiro <nonaka@netbsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: nvme_pci.c,v 1.36 2022/07/09 01:24:32 pgoyette Exp $");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/device.h>
#include <sys/bitops.h>
#include <sys/bus.h>
#include <sys/cpu.h>
#include <sys/interrupt.h>
#include <sys/kmem.h>
#include <sys/pmf.h>
#include <sys/module.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcidevs.h>

#include <dev/ic/nvmereg.h>
#include <dev/ic/nvmevar.h>

int nvme_pci_force_intx = 0;
int nvme_pci_mpsafe = 1;
int nvme_pci_mq = 1;		/* INTx: ioq=1, MSI/MSI-X: ioq=ncpu */

#define NVME_PCI_BAR		0x10

struct nvme_pci_softc {
	struct nvme_softc	psc_nvme;

	pci_chipset_tag_t	psc_pc;
	pci_intr_handle_t	*psc_intrs;
	int			psc_nintrs;
};

static int	nvme_pci_match(device_t, cfdata_t, void *);
static void	nvme_pci_attach(device_t, device_t, void *);
static int	nvme_pci_detach(device_t, int);
static int	nvme_pci_rescan(device_t, const char *, const int *);
static bool	nvme_pci_suspend(device_t, const pmf_qual_t *);
static bool	nvme_pci_resume(device_t, const pmf_qual_t *);

CFATTACH_DECL3_NEW(nvme_pci, sizeof(struct nvme_pci_softc),
    nvme_pci_match, nvme_pci_attach, nvme_pci_detach, NULL, nvme_pci_rescan,
    nvme_childdet, DVF_DETACH_SHUTDOWN);

static int	nvme_pci_intr_establish(struct nvme_softc *,
		    uint16_t, struct nvme_queue *);
static int	nvme_pci_intr_disestablish(struct nvme_softc *, uint16_t);
static int	nvme_pci_setup_intr(struct pci_attach_args *,
		    struct nvme_pci_softc *);

static const struct nvme_pci_quirk {
	pci_vendor_id_t		vendor;
	pci_product_id_t	product;
	uint32_t		quirks;
} nvme_pci_quirks[] = {
	{ PCI_VENDOR_HGST, PCI_PRODUCT_HGST_SN100,
	    NVME_QUIRK_DELAY_B4_CHK_RDY },
	{ PCI_VENDOR_HGST, PCI_PRODUCT_HGST_SN200,
	    NVME_QUIRK_DELAY_B4_CHK_RDY },
	{ PCI_VENDOR_BEIJING_MEMBLAZE, PCI_PRODUCT_BEIJING_MEMBLAZE_PBLAZE4,
	    NVME_QUIRK_DELAY_B4_CHK_RDY },
	{ PCI_VENDOR_SAMSUNGELEC3, PCI_PRODUCT_SAMSUNGELEC3_172X,
	    NVME_QUIRK_DELAY_B4_CHK_RDY },
	{ PCI_VENDOR_SAMSUNGELEC3, PCI_PRODUCT_SAMSUNGELEC3_172XAB,
	    NVME_QUIRK_DELAY_B4_CHK_RDY },
	{ PCI_VENDOR_INTEL, PCI_PRODUCT_INTEL_DC_P4500_SSD,
	    NVME_QUIRK_NOMSI },
};

static const struct nvme_pci_quirk *
nvme_pci_lookup_quirk(struct pci_attach_args *pa)
{
	const struct nvme_pci_quirk *q;
	int i;

	for (i = 0; i < __arraycount(nvme_pci_quirks); i++) {
		q = &nvme_pci_quirks[i];

		if (PCI_VENDOR(pa->pa_id) == q->vendor &&
		    PCI_PRODUCT(pa->pa_id) == q->product)
			return q;
	}
	return NULL;
}

static int
nvme_pci_match(device_t parent, cfdata_t match, void *aux)
{
	struct pci_attach_args *pa = aux;

	if (PCI_CLASS(pa->pa_class) == PCI_CLASS_MASS_STORAGE &&
	    PCI_SUBCLASS(pa->pa_class) == PCI_SUBCLASS_MASS_STORAGE_NVM &&
	    PCI_INTERFACE(pa->pa_class) == PCI_INTERFACE_NVM_NVME_IO)
		return 1;

	return 0;
}

static void
nvme_pci_attach(device_t parent, device_t self, void *aux)
{
	struct nvme_pci_softc *psc = device_private(self);
	struct nvme_softc *sc = &psc->psc_nvme;
	struct pci_attach_args *pa = aux;
	const struct nvme_pci_quirk *quirk;
	pcireg_t memtype, reg;
	bus_addr_t memaddr;
	int flags, error;
	int msixoff;

	sc->sc_dev = self;
	psc->psc_pc = pa->pa_pc;
	if (pci_dma64_available(pa))
		sc->sc_dmat = pa->pa_dmat64;
	else
		sc->sc_dmat = pa->pa_dmat;

	quirk = nvme_pci_lookup_quirk(pa);
	if (quirk != NULL)
		sc->sc_quirks = quirk->quirks;

	pci_aprint_devinfo(pa, NULL);

	/* Map registers */
	memtype = pci_mapreg_type(pa->pa_pc, pa->pa_tag, NVME_PCI_BAR);
	if (PCI_MAPREG_TYPE(memtype) != PCI_MAPREG_TYPE_MEM) {
		aprint_error_dev(self, "invalid type (type=0x%x)\n", memtype);
		return;
	}
	reg = pci_conf_read(pa->pa_pc, pa->pa_tag, PCI_COMMAND_STATUS_REG);
	if (((reg & PCI_COMMAND_MASTER_ENABLE) == 0) ||
	    ((reg & PCI_COMMAND_MEM_ENABLE) == 0)) {
		/*
		 * Enable address decoding for memory range in case BIOS or
		 * UEFI didn't set it.
		 */
		reg |= PCI_COMMAND_MASTER_ENABLE | PCI_COMMAND_MEM_ENABLE;
        	pci_conf_write(pa->pa_pc, pa->pa_tag, PCI_COMMAND_STATUS_REG,
		    reg);
	}

	sc->sc_iot = pa->pa_memt;
	error = pci_mapreg_info(pa->pa_pc, pa->pa_tag, NVME_PCI_BAR,
	    memtype, &memaddr, &sc->sc_ios, &flags);
	if (error) {
		aprint_error_dev(self, "can't get map info\n");
		return;
	}

	if (pci_get_capability(pa->pa_pc, pa->pa_tag, PCI_CAP_MSIX, &msixoff,
	    NULL)) {
		pcireg_t msixtbl;
		uint32_t table_offset;
		int bir;

		msixtbl = pci_conf_read(pa->pa_pc, pa->pa_tag,
		    msixoff + PCI_MSIX_TBLOFFSET);
		table_offset = msixtbl & PCI_MSIX_TBLOFFSET_MASK;
		bir = msixtbl & PCI_MSIX_TBLBIR_MASK;
		if (bir == PCI_MAPREG_NUM(NVME_PCI_BAR)) {
			sc->sc_ios = table_offset;
		}
	}

	error = bus_space_map(sc->sc_iot, memaddr, sc->sc_ios, flags,
	    &sc->sc_ioh);
	if (error != 0) {
		aprint_error_dev(self, "can't map mem space (error=%d)\n",
		    error);
		return;
	}

	/* Establish interrupts */
	if (nvme_pci_setup_intr(pa, psc) != 0) {
		aprint_error_dev(self, "unable to allocate interrupt\n");
		goto unmap;
	}
	sc->sc_intr_establish = nvme_pci_intr_establish;
	sc->sc_intr_disestablish = nvme_pci_intr_disestablish;

	sc->sc_ih = kmem_zalloc(sizeof(*sc->sc_ih) * psc->psc_nintrs, KM_SLEEP);
	sc->sc_softih = kmem_zalloc(
	    sizeof(*sc->sc_softih) * psc->psc_nintrs, KM_SLEEP);

	if (nvme_attach(sc) != 0) {
		/* error printed by nvme_attach() */
		goto softintr_free;
	}

	if (!pmf_device_register(self, nvme_pci_suspend, nvme_pci_resume))
		aprint_error_dev(self, "couldn't establish power handler\n");

	SET(sc->sc_flags, NVME_F_ATTACHED);
	return;

softintr_free:
	kmem_free(sc->sc_softih, sizeof(*sc->sc_softih) * psc->psc_nintrs);
	kmem_free(sc->sc_ih, sizeof(*sc->sc_ih) * psc->psc_nintrs);
	sc->sc_nq = 0;
	pci_intr_release(pa->pa_pc, psc->psc_intrs, psc->psc_nintrs);
	psc->psc_nintrs = 0;
unmap:
	bus_space_unmap(sc->sc_iot, sc->sc_ioh, sc->sc_ios);
	sc->sc_ios = 0;
}

static int
nvme_pci_rescan(device_t self, const char *attr, const int *flags)
{

	return nvme_rescan(self, attr, flags);
}

static bool
nvme_pci_suspend(device_t self, const pmf_qual_t *qual)
{
	struct nvme_pci_softc *psc = device_private(self);
	struct nvme_softc *sc = &psc->psc_nvme;
	int error;

	error = nvme_suspend(sc);
	if (error)
		return false;

	return true;
}

static bool
nvme_pci_resume(device_t self, const pmf_qual_t *qual)
{
	struct nvme_pci_softc *psc = device_private(self);
	struct nvme_softc *sc = &psc->psc_nvme;
	int error;

	error = nvme_resume(sc);
	if (error)
		return false;

	return true;
}

static int
nvme_pci_detach(device_t self, int flags)
{
	struct nvme_pci_softc *psc = device_private(self);
	struct nvme_softc *sc = &psc->psc_nvme;
	int error;

	if (!ISSET(sc->sc_flags, NVME_F_ATTACHED))
		return 0;

	error = nvme_detach(sc, flags);
	if (error)
		return error;

	kmem_free(sc->sc_softih, sizeof(*sc->sc_softih) * psc->psc_nintrs);
	sc->sc_softih = NULL;

	kmem_free(sc->sc_ih, sizeof(*sc->sc_ih) * psc->psc_nintrs);
	pci_intr_release(psc->psc_pc, psc->psc_intrs, psc->psc_nintrs);
	bus_space_unmap(sc->sc_iot, sc->sc_ioh, sc->sc_ios);
	return 0;
}

static int
nvme_pci_intr_establish(struct nvme_softc *sc, uint16_t qid,
    struct nvme_queue *q)
{
	struct nvme_pci_softc *psc = (struct nvme_pci_softc *)sc;
	char intr_xname[INTRDEVNAMEBUF];
	char intrbuf[PCI_INTRSTR_LEN];
	const char *intrstr = NULL;
	int (*ih_func)(void *);
	void (*ih_func_soft)(void *);
	void *ih_arg;
	int error;

	KASSERT(sc->sc_use_mq || qid == NVME_ADMIN_Q);
	KASSERT(sc->sc_ih[qid] == NULL);

	if (nvme_pci_mpsafe) {
		pci_intr_setattr(psc->psc_pc, &psc->psc_intrs[qid],
		    PCI_INTR_MPSAFE, true);
	}

	if (!sc->sc_use_mq) {
		snprintf(intr_xname, sizeof(intr_xname), "%s",
		    device_xname(sc->sc_dev));
		ih_arg = sc;
		ih_func = nvme_intr;
		ih_func_soft = nvme_softintr_intx;
	} else {
		if (qid == NVME_ADMIN_Q) {
			snprintf(intr_xname, sizeof(intr_xname), "%s adminq",
			    device_xname(sc->sc_dev));
		} else {
			snprintf(intr_xname, sizeof(intr_xname), "%s ioq%d",
			    device_xname(sc->sc_dev), qid);
		}
		ih_arg = q;
		ih_func = nvme_intr_msi;
		ih_func_soft = nvme_softintr_msi;
	}

	/* establish hardware interrupt */
	sc->sc_ih[qid] = pci_intr_establish_xname(psc->psc_pc,
	    psc->psc_intrs[qid], IPL_BIO, ih_func, ih_arg, intr_xname);
	if (sc->sc_ih[qid] == NULL) {
		aprint_error_dev(sc->sc_dev,
		    "unable to establish %s interrupt\n", intr_xname);
		return 1;
	}

	/* establish also the software interrupt */
	sc->sc_softih[qid] = softint_establish(
	    SOFTINT_BIO|(nvme_pci_mpsafe ? SOFTINT_MPSAFE : 0),
	    ih_func_soft, q);
	if (sc->sc_softih[qid] == NULL) {
		pci_intr_disestablish(psc->psc_pc, sc->sc_ih[qid]);
		sc->sc_ih[qid] = NULL;

		aprint_error_dev(sc->sc_dev,
		    "unable to establish %s soft interrupt\n",
		    intr_xname);
		return 1;
	}

	intrstr = pci_intr_string(psc->psc_pc, psc->psc_intrs[qid], intrbuf,
	    sizeof(intrbuf));
	if (!sc->sc_use_mq) {
		aprint_normal_dev(sc->sc_dev, "interrupting at %s\n", intrstr);
	} else if (qid == NVME_ADMIN_Q) {
		aprint_normal_dev(sc->sc_dev,
		    "for admin queue interrupting at %s\n", intrstr);
	} else if (!nvme_pci_mpsafe) {
		aprint_normal_dev(sc->sc_dev,
		    "for io queue %d interrupting at %s\n", qid, intrstr);
	} else {
		kcpuset_t *affinity;
		cpuid_t affinity_to;

		kcpuset_create(&affinity, true);
		affinity_to = (qid - 1) % ncpu;
		kcpuset_set(affinity, affinity_to);
		error = interrupt_distribute(sc->sc_ih[qid], affinity, NULL);
		kcpuset_destroy(affinity);
		aprint_normal_dev(sc->sc_dev,
		    "for io queue %d interrupting at %s", qid, intrstr);
		if (error == 0)
			aprint_normal(" affinity to cpu%lu", affinity_to);
		aprint_normal("\n");
	}
	return 0;
}

static int
nvme_pci_intr_disestablish(struct nvme_softc *sc, uint16_t qid)
{
	struct nvme_pci_softc *psc = (struct nvme_pci_softc *)sc;

	KASSERT(sc->sc_use_mq || qid == NVME_ADMIN_Q);
	KASSERT(sc->sc_ih[qid] != NULL);

	if (sc->sc_softih) {
		softint_disestablish(sc->sc_softih[qid]);
		sc->sc_softih[qid] = NULL;
	}

	pci_intr_disestablish(psc->psc_pc, sc->sc_ih[qid]);
	sc->sc_ih[qid] = NULL;

	return 0;
}

static int
nvme_pci_setup_intr(struct pci_attach_args *pa, struct nvme_pci_softc *psc)
{
	struct nvme_softc *sc = &psc->psc_nvme;
	int error;
	int counts[PCI_INTR_TYPE_SIZE];
	pci_intr_handle_t *ihps;
	int intr_type;

	memset(counts, 0, sizeof(counts));

	if (nvme_pci_force_intx)
		goto setup_intx;

	/* MSI-X */
	counts[PCI_INTR_TYPE_MSIX] = uimin(pci_msix_count(pa->pa_pc, pa->pa_tag),
	    ncpu + 1);
	if (counts[PCI_INTR_TYPE_MSIX] < 1) {
		counts[PCI_INTR_TYPE_MSIX] = 0;
	} else if (!nvme_pci_mq || !nvme_pci_mpsafe) {
		if (counts[PCI_INTR_TYPE_MSIX] > 2)
			counts[PCI_INTR_TYPE_MSIX] = 2;	/* adminq + 1 ioq */
	}

	/* MSI */
	if (sc->sc_quirks & NVME_QUIRK_NOMSI)
		goto setup_intx;
	counts[PCI_INTR_TYPE_MSI] = pci_msi_count(pa->pa_pc, pa->pa_tag);
	if (counts[PCI_INTR_TYPE_MSI] > 0) {
		while (counts[PCI_INTR_TYPE_MSI] > ncpu + 1) {
			if (counts[PCI_INTR_TYPE_MSI] / 2 <= ncpu + 1)
				break;
			counts[PCI_INTR_TYPE_MSI] /= 2;
		}
	}
	if (counts[PCI_INTR_TYPE_MSI] < 1) {
		counts[PCI_INTR_TYPE_MSI] = 0;
	} else if (!nvme_pci_mq || !nvme_pci_mpsafe) {
		if (counts[PCI_INTR_TYPE_MSI] > 2)
			counts[PCI_INTR_TYPE_MSI] = 2;	/* adminq + 1 ioq */
	}

setup_intx:
	/* INTx */
	counts[PCI_INTR_TYPE_INTX] = 1;

	error = pci_intr_alloc(pa, &ihps, counts, PCI_INTR_TYPE_MSIX);
	if (error)
		return error;

	intr_type = pci_intr_type(pa->pa_pc, ihps[0]);

	psc->psc_intrs = ihps;
	psc->psc_nintrs = counts[intr_type];
	if (intr_type == PCI_INTR_TYPE_MSI) {
		if (counts[intr_type] > ncpu + 1)
			counts[intr_type] = ncpu + 1;
	}
	sc->sc_use_mq = counts[intr_type] > 1;
	sc->sc_nq = sc->sc_use_mq ? counts[intr_type] - 1 : 1;

	return 0;
}

MODULE(MODULE_CLASS_DRIVER, nvme, "pci,dk_subr");

#ifdef _MODULE
#include "ioconf.c"
#endif

static int
nvme_modcmd(modcmd_t cmd, void *opaque)
{
#ifdef _MODULE
	devmajor_t cmajor, bmajor;
	extern const struct cdevsw nvme_cdevsw;
	static bool devsw_ok;
#endif
	int error = 0;

#ifdef _MODULE
	switch (cmd) {
	case MODULE_CMD_INIT:
		bmajor = cmajor = NODEVMAJOR;
		error = devsw_attach(nvme_cd.cd_name, NULL, &bmajor,
		    &nvme_cdevsw, &cmajor);
		if (error) {
			/*XXXPRG devsw_ok = false;*/
			aprint_error("%s: unable to register devsw, err %d\n",
			    nvme_cd.cd_name, error);
			/* do not abort, just /dev/nvme* will not work */
		}
		else
			devsw_ok = true;

		error = config_init_component(cfdriver_ioconf_nvme_pci,
		    cfattach_ioconf_nvme_pci, cfdata_ioconf_nvme_pci);
		if (error) {
			if (devsw_ok) {
				devsw_detach(NULL, &nvme_cdevsw);
				devsw_ok = false;
			}
			break;
		}
		break;
	case MODULE_CMD_FINI:
		error = config_fini_component(cfdriver_ioconf_nvme_pci,
		    cfattach_ioconf_nvme_pci, cfdata_ioconf_nvme_pci);
		if (devsw_ok) {
			devsw_detach(NULL, &nvme_cdevsw);
			devsw_ok = false;
		}
		break;
	default:
		break;
	}
#endif
	return error;
}
