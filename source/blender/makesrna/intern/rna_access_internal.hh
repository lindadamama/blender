/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#pragma once

#include "rna_internal_types.hh"

struct IDProperty;
struct PropertyRNAOrID;

/**
 * This function initializes a #PropertyRNAOrID with all required info, from a given #PropertyRNA
 * and #PointerRNA data. It deals properly with the three cases
 * (static RNA, runtime RNA, and #IDProperty).
 * \warning given `ptr` #PointerRNA is assumed to be a valid data one here, calling code is
 * responsible to ensure that.
 */
void rna_property_rna_or_id_get(PropertyRNA *prop,
                                PointerRNA *ptr,
                                PropertyRNAOrID *r_prop_rna_or_id);

void rna_idproperty_touch(IDProperty *idprop);
IDProperty *rna_idproperty_find(PointerRNA *ptr, const char *name);

IDProperty *rna_system_idproperty_find(PointerRNA *ptr, const char *name);

/**
 * Find the property which uses the given nested struct.
 */
PropertyRNA *rna_struct_find_nested(PointerRNA *ptr, StructRNA *srna);
