# Upstream PRoot CI (disabled in this fork)

These are the GitHub Actions workflows inherited from
[PRoot](https://github.com/proot-me/proot).  They are kept here for reference
but are **not active**: they target upstream infrastructure that this fork does
not own (`ghcr.io/proot-me/*`, the upstream GitLab pipelines, upstream release
automation), so running them here would only fail.

They live outside `.github/workflows/` on purpose: publishing a token without
the `workflow` scope cannot create or update files under that path.

To re-enable CI, move them back with a token that has the `workflow` scope:

```sh
mkdir -p .github/workflows
git mv contrib/ci-upstream/*.yml .github/workflows/
git commit -m "ci: restore upstream workflows"
git push
```
