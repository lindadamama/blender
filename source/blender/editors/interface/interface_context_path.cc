/* SPDX-FileCopyrightText: 2021 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "BLI_vector.hh"

#include "RNA_access.hh"

#include "UI_interface.hh"
#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_prototypes.hh"

namespace blender::ui {

void context_path_add_generic(Vector<ContextPathItem> &path,
                              StructRNA &rna_type,
                              void *ptr,
                              const BIFIconID icon_override)
{
  /* Add the null check here to make calling functions less verbose. */
  if (!ptr) {
    return;
  }

  PointerRNA rna_ptr = RNA_pointer_create_discrete(nullptr, &rna_type, ptr);
  char name_buf[128], *name;
  name = RNA_struct_name_get_alloc(&rna_ptr, name_buf, sizeof(name_buf), nullptr);

  /* Use a blank icon by default to check whether to retrieve it automatically from the type. */
  const BIFIconID icon = icon_override == ICON_NONE ? RNA_struct_ui_icon(rna_ptr.type) :
                                                      icon_override;

  if (&rna_type == &RNA_NodeTree) {
    ID *id = (ID *)ptr;
    path.append({name, icon, ID_REAL_USERS(id)});
  }
  else {
    path.append({name, icon, 1});
  }
  if (name != name_buf) {
    MEM_freeN(name);
  }
}

/* -------------------------------------------------------------------- */
/** \name Breadcrumb Template
 * \{ */

void template_breadcrumbs(uiLayout &layout, Span<ContextPathItem> context_path)
{
  uiLayout *row = &layout.row(true);
  layout.alignment_set(LayoutAlign::Left);

  for (const int i : context_path.index_range()) {
    uiLayout *sub_row = &row->row(true);
    sub_row->alignment_set(LayoutAlign::Left);

    if (i > 0) {
      sub_row->label("", ICON_RIGHTARROW_THIN);
    }
    uiBut *but = uiItemL_ex(
        sub_row, context_path[i].name.c_str(), context_path[i].icon, false, false);
    UI_but_icon_indicator_number_set(but, context_path[i].icon_indicator_number);
  }
}

/** \} */

}  // namespace blender::ui
