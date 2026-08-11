# Configured placement items

Mods can associate a reusable miscellaneous item with a placeable object by
adding a JSON file directly under `Data` whose name ends in
`_PlacementItem_SP.json`.

```json
[
  {
    "Item": {
      "FileName": "MyMod.esp",
      "LocalFormID": "800"
    },
    "Object": {
      "FileName": "MyMod.esp",
      "LocalFormID": "801"
    }
  }
]
```

`LocalFormID` is a hexadecimal string and may optionally start with `0x`. It
must not include a load-order prefix. `Item` must resolve to a `MISC` form and
`Object` must resolve to a bound object.

Dropping the configured item creates the mapped object and starts placement.
Picking up one mapped object returns the configured item when the object is not
a container. Containers and multi-object groups continue to use SkyPlace's
dynamic items so their unique state can be preserved.
