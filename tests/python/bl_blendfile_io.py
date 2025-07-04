# SPDX-FileCopyrightText: 2020-2023 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# ./blender.bin --background --python tests/python/bl_blendfile_io.py
import bpy
import os
import sys

sys.path.append(os.path.dirname(os.path.realpath(__file__)))
from bl_blendfile_utils import TestHelper


class TestBlendFileSaveLoadBasic(TestHelper):

    def __init__(self, args):
        super().__init__(args)

    def test_save_load(self):
        bpy.ops.wm.read_homefile(use_empty=True, use_factory_startup=True)

        bpy.data.meshes.new("OrphanedMesh")

        output_dir = self.args.output_dir
        self.ensure_path(output_dir)

        # Take care to keep the name unique so multiple test jobs can run at once.
        output_path = os.path.join(output_dir, "blendfile_io.blend")

        orig_data = self.blender_data_to_tuple(bpy.data, "orig_data 1")

        bpy.ops.wm.save_as_mainfile(filepath=output_path, check_existing=False, compress=False)
        bpy.ops.wm.open_mainfile(filepath=output_path, load_ui=False)

        read_data = self.blender_data_to_tuple(bpy.data, "read_data 1")

        # We have orphaned data, which should be removed by file reading, so there should not be equality here.
        self.assertNotEqual(orig_data, read_data)

        bpy.data.orphans_purge()

        orig_data = self.blender_data_to_tuple(bpy.data, "orig_data 2")

        bpy.ops.wm.save_as_mainfile(filepath=output_path, check_existing=False, compress=False)
        bpy.ops.wm.open_mainfile(filepath=output_path, load_ui=False)

        read_data = self.blender_data_to_tuple(bpy.data, "read_data 2")

        self.assertEqual(orig_data, read_data)


class TestBlendFileSavePartial(TestHelper):
    OBJECT_MESH_NAME = "ObjectMesh"
    OBJECT_MATERIAL_NAME = "ObjectMaterial"
    OBJECT_NAME = "Object"
    UNUSED_MESH_NAME = "UnusedMesh"

    def __init__(self, args):
        super().__init__(args)

    def test_save_load(self):
        bpy.ops.wm.read_homefile(use_empty=True, use_factory_startup=True)

        ob_mesh = bpy.data.meshes.new(self.OBJECT_MESH_NAME)
        ob_material = bpy.data.materials.new(self.OBJECT_MATERIAL_NAME)
        ob_mesh.materials.append(ob_material)
        ob = bpy.data.objects.new(self.OBJECT_NAME, object_data=ob_mesh)
        bpy.context.collection.objects.link(ob)

        unused_mesh = bpy.data.meshes.new(self.UNUSED_MESH_NAME)
        unused_mesh.materials.append(ob_material)

        self.assertEqual(ob_mesh.users, 1)
        self.assertEqual(ob_material.users, 2)
        self.assertEqual(ob.users, 1)
        self.assertEqual(unused_mesh.users, 0)

        output_dir = self.args.output_dir
        self.ensure_path(output_dir)

        # Take care to keep the name unique so multiple test jobs can run at once.
        output_path = os.path.join(output_dir, "blendfile_io_partial.blend")

        bpy.data.libraries.write(filepath=output_path, datablocks={ob, unused_mesh}, fake_user=False)
        bpy.ops.wm.open_mainfile(filepath=output_path, load_ui=False)

        self.assertIn(self.OBJECT_MESH_NAME, bpy.data.meshes)
        self.assertIn(self.OBJECT_MATERIAL_NAME, bpy.data.materials)
        self.assertIn(self.OBJECT_NAME, bpy.data.objects)
        self.assertIn(self.UNUSED_MESH_NAME, bpy.data.meshes)

        self.assertEqual(bpy.data.meshes[self.OBJECT_MESH_NAME].users, 1)
        self.assertEqual(bpy.data.materials[self.OBJECT_MATERIAL_NAME].users, 2)
        self.assertEqual(bpy.data.objects[self.OBJECT_NAME].users, 0)
        self.assertEqual(bpy.data.meshes[self.UNUSED_MESH_NAME].users, 0)


# NOTE: Technically this should rather be in `bl_id_management.py` test, but that file uses `unittest` module,
#       which makes mixing it with tests system used here and passing extra parameters complicated.
#       Since the main effect of 'RUNTIME' ID tag is on file save, it can as well be here for now.
class TestIdRuntimeTag(TestHelper):

    def __init__(self, args):
        super().__init__(args)

    def unique_blendfile_name(self, base_name):
        return base_name + self.__class__.__name__ + ".blend"

    def test_basics(self):
        output_dir = self.args.output_dir
        self.ensure_path(output_dir)
        bpy.ops.wm.read_homefile(use_empty=False, use_factory_startup=True)

        obj = bpy.data.objects['Cube']
        self.assertFalse(obj.is_runtime_data)
        self.assertTrue(bpy.context.view_layer.depsgraph.ids['Cube'].is_runtime_data)

        output_work_path = os.path.join(output_dir, self.unique_blendfile_name("blendfile"))
        bpy.ops.wm.save_as_mainfile(filepath=output_work_path, check_existing=False, compress=False)

        bpy.ops.wm.open_mainfile(filepath=output_work_path, load_ui=False)
        obj = bpy.data.objects['Cube']
        self.assertFalse(obj.is_runtime_data)

        obj.is_runtime_data = True
        self.assertTrue(obj.is_runtime_data)

        bpy.ops.wm.save_as_mainfile(filepath=output_work_path, check_existing=False, compress=False)
        bpy.ops.wm.open_mainfile(filepath=output_work_path, load_ui=False)

        self.assertNotIn('Cube', bpy.data.objects)
        mesh = bpy.data.meshes['Cube']
        self.assertFalse(mesh.is_runtime_data)
        self.assertEqual(mesh.users, 0)

    def test_linking(self):
        output_dir = self.args.output_dir
        self.ensure_path(output_dir)
        bpy.ops.wm.read_homefile(use_empty=False, use_factory_startup=True)

        material = bpy.data.materials.new("LibMaterial")
        # Use a dummy mesh as user of the material, such that the material is saved
        # without having to use fake user on it.
        mesh = bpy.data.meshes.new("LibMesh")
        mesh.materials.append(material)
        mesh.use_fake_user = True

        output_lib_path = os.path.join(output_dir, self.unique_blendfile_name("blendlib_runtimetag_basic"))
        bpy.ops.wm.save_as_mainfile(filepath=output_lib_path, check_existing=False, compress=False)

        bpy.ops.wm.read_homefile(use_empty=False, use_factory_startup=True)

        obj = bpy.data.objects['Cube']
        self.assertFalse(obj.is_runtime_data)
        obj.is_runtime_data = True

        link_dir = os.path.join(output_lib_path, "Material")
        bpy.ops.wm.link(directory=link_dir, filename="LibMaterial")

        linked_material = bpy.data.materials['LibMaterial']
        self.assertFalse(linked_material.is_library_indirect)

        link_dir = os.path.join(output_lib_path, "Mesh")
        bpy.ops.wm.link(directory=link_dir, filename="LibMesh", instance_object_data=False)

        linked_mesh = bpy.data.meshes['LibMesh']
        self.assertFalse(linked_mesh.is_library_indirect)
        self.assertFalse(linked_mesh.use_fake_user)

        obj.data = linked_mesh
        obj.material_slots[0].link = 'OBJECT'
        obj.material_slots[0].material = linked_material

        output_work_path = os.path.join(output_dir, self.unique_blendfile_name("blendfile"))
        bpy.ops.wm.save_as_mainfile(filepath=output_work_path, check_existing=False, compress=False)

        # Only usage of this linked material is a runtime ID (object),
        # so writing .blend file will have properly reset its tag to indirectly linked data.
        self.assertTrue(linked_material.is_library_indirect)

        # Only usage of this linked mesh is a runtime ID (object),
        # so writing .blend file will have properly reset its tag to indirectly linked data.
        self.assertTrue(linked_mesh.is_library_indirect)

        bpy.ops.wm.open_mainfile(filepath=output_work_path, load_ui=False)

        self.assertNotIn('Cube', bpy.data.objects)
        self.assertNotIn('LibMaterial', bpy.data.materials)
        self.assertNotIn('LibMesh', bpy.data.meshes)


TESTS = (
    TestBlendFileSaveLoadBasic,
    TestBlendFileSavePartial,

    TestIdRuntimeTag,
)


def argparse_create():
    import argparse

    # When --help or no args are given, print this help
    description = "Test basic IO of blend file."
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument(
        "--output-dir",
        dest="output_dir",
        default=".",
        help="Where to output temp saved blendfiles",
        required=False,
    )

    return parser


def main():
    args = argparse_create().parse_args()

    # Don't write thumbnails into the home directory.
    bpy.context.preferences.filepaths.file_preview_type = 'NONE'

    for Test in TESTS:
        Test(args).run_all_tests()


if __name__ == '__main__':
    import sys
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    main()
