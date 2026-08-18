# Feature name

FFlag: LuauExportsAreConst

## Summary

Amends the export-by-value RFC described in [Luau RFC #179](<https://github.com/luau-lang/rfcs/pull/179>) making all exports-by-value constant, removing the `export local` syntax and with it marking exports as mutable.

## Motivation

Exports-by-value should not be mutable. First and foremost, it greatly simplifies any optimizations which utilize exports-by-value, and secondly removes the objectively ugly syntax of `export local`.

## Design

Exports-by-value are now only declared via the syntax of
```luau
export variable_name[: type] = value
```
instead of optionally accepting the `local` keyword to signify that they're mutable by the module in which they're declared prior to them being returned at the end of the file.
This makes all exports-by-value constant with no way to change it.

## Drawbacks

Breaks backwards compatibility with any code that utilizes `export local`.

## Alternatives (optional)

Not doing this.
