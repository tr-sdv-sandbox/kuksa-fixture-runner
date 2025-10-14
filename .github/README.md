# GitHub Actions CI/CD Setup

This project uses GitHub Actions to automatically build, test, and publish Docker images.

## Workflow Overview

The CI/CD pipeline is defined in `.github/workflows/ci.yml` and runs on:
- Push to `main` branch
- Pull requests to `main` branch
- Semantic version tags (v1.0.0, v2.1.3, etc.)

## Pipeline Jobs

### 1. Build
Compiles fixture-runner from source:
- Installs dependencies
- Builds libkuksa-cpp from GitHub
- Compiles fixture-runner
- Uploads build artifacts for testing

### 2. Test (Integration Tests)
Runs integration tests with real KUKSA databroker:
- Downloads build artifacts
- Installs runtime dependencies
- Starts KUKSA databroker in Docker
- Runs `test_fixture_runner`
- Tests actuation and feedback loops

### 3. Docker
Builds and optionally pushes Docker image:
- **Pull Requests**: Build only (no push)
- **Main branch**: Build and push with branch tag
- **Version tags**: Build and push with semantic version tags

Uses GitHub Actions cache for faster builds.

### 4. Release
Creates GitHub Release (only on version tags):
- Extracts version from tag
- Creates release with auto-generated notes
- Includes Docker pull instructions

## Required Permissions

**No additional secrets required!**

GitHub Actions automatically provides:
- `GITHUB_TOKEN` with appropriate permissions
- Access to GitHub Container Registry (ghcr.io)

The workflow declares these permissions:
```yaml
permissions:
  contents: write    # For creating releases
  packages: write    # For pushing to GHCR
```

## Triggering Workflows

### Development Workflow
```bash
# Regular commits trigger build + test
git add .
git commit -m "feat: add new feature"
git push origin main
```

### Pull Request Workflow
```bash
# Create PR - builds and tests but doesn't push image
git checkout -b feature/new-feature
git commit -am "Add new feature"
git push origin feature/new-feature
# Create PR on GitHub
```

### Release Workflow
```bash
# Create semantic version tag to trigger full pipeline
git tag v1.0.0
git push origin v1.0.0

# Or use annotated tags
git tag -a v1.0.0 -m "Release version 1.0.0"
git push origin v1.0.0
```

## Semantic Versioning

Follow [Semantic Versioning 2.0.0](https://semver.org/):

- **MAJOR** version (v2.0.0): Incompatible API changes
- **MINOR** version (v1.1.0): Backwards-compatible new features
- **PATCH** version (v1.0.1): Backwards-compatible bug fixes

Valid tags:
- `v1.0.0` ✓ - Full release
- `v1.2.3` ✓ - Patch release
- `v2.0.0` ✓ - Major release

Invalid tags (won't trigger release):
- `1.0.0` ✗ - Missing 'v' prefix
- `v1.0` ✗ - Incomplete version
- `v1.0.0-beta` ✗ - Pre-release suffix

## Published Docker Images

After successful workflow runs, images are available at:

```bash
# Pull latest from main branch
docker pull ghcr.io/YOUR_ORG/kuksa-fixture-runner:main

# Pull specific version
docker pull ghcr.io/YOUR_ORG/kuksa-fixture-runner:v1.0.0
docker pull ghcr.io/YOUR_ORG/kuksa-fixture-runner:1.0.0
docker pull ghcr.io/YOUR_ORG/kuksa-fixture-runner:1.0
docker pull ghcr.io/YOUR_ORG/kuksa-fixture-runner:1

# Pull by commit SHA
docker pull ghcr.io/YOUR_ORG/kuksa-fixture-runner:main-abc1234

# Run
docker run --rm \
  -e KUKSA_ADDRESS=databroker:55555 \
  -v $(pwd)/fixtures.json:/app/fixtures.json:ro \
  ghcr.io/YOUR_ORG/kuksa-fixture-runner:v1.0.0 \
  fixture-runner --config /app/fixtures.json
```

## Workflow Status Badges

Add to your README.md:

```markdown
[![CI/CD Pipeline](https://github.com/YOUR_ORG/kuksa-fixture-runner/actions/workflows/ci.yml/badge.svg)](https://github.com/YOUR_ORG/kuksa-fixture-runner/actions/workflows/ci.yml)
```

## Container Registry Configuration

### Making Images Public

By default, images inherit repository visibility. To make them public:

1. Go to repository → Packages → kuksa-fixture-runner
2. Click "Package settings"
3. Scroll to "Danger Zone"
4. Click "Change visibility" → "Public"

### Package Permissions

GitHub automatically grants:
- **Repository owner**: Admin access
- **Repository collaborators**: Write access (if repo is private)
- **Public access**: Read access (if package is public)

To customize:
1. Go to Package settings
2. Manage Actions access
3. Add specific repositories or users

## Local Testing

### Test workflow locally with `act`

Install [act](https://github.com/nektos/act):
```bash
# Install act
curl https://raw.githubusercontent.com/nektos/act/master/install.sh | sudo bash

# Run workflow locally
act push

# Run specific job
act -j build

# Run with specific event
act push --eventpath event.json
```

### Manual build and test
```bash
# Build
mkdir -p build && cd build
cmake ..
make -j$(nproc)

# Run tests
cd tests/integration
./test_fixture_runner
```

## Troubleshooting

### Integration tests fail in CI
**Symptom**: Tests pass locally but fail in GitHub Actions

**Solutions**:
- Check Docker is available: GitHub runners have Docker pre-installed
- Verify test timeouts: CI may be slower than local
- Check logs: Click on failed job → Expand test step
- Run locally with `act` to reproduce

### Docker push fails
**Symptom**: `denied: permission_denied`

**Solutions**:
- Verify workflow has `packages: write` permission
- Check repository settings → Actions → General → Workflow permissions
- Ensure "Read and write permissions" is enabled
- For organization repos, check org-level restrictions

### Build artifacts not found
**Symptom**: `test` job can't find build artifacts

**Solutions**:
- Verify `build` job completed successfully
- Check artifact retention (default: 1 day)
- Ensure upload/download artifact names match
- Look for upload/download errors in logs

### Tag doesn't trigger release
**Symptom**: No release created after pushing tag

**Solutions**:
- Must match pattern: `v*.*.*` (e.g., v1.0.0)
- Check workflow runs: Actions tab → All workflows
- Verify `contents: write` permission
- Check if tag is on main branch (or configured branch)

### Cache issues
**Symptom**: Builds are slow or cache errors occur

**Solutions**:
```bash
# Clear GitHub Actions cache
gh cache delete --all

# Or via GitHub UI
# Settings → Actions → Caches → Delete all caches
```

## Advanced Configuration

### Build matrix for multiple platforms
```yaml
strategy:
  matrix:
    os: [ubuntu-latest, ubuntu-22.04]
    arch: [amd64, arm64]
```

### Multi-architecture Docker images
```yaml
- name: Set up QEMU
  uses: docker/setup-qemu-action@v3

- name: Build multi-arch image
  uses: docker/build-push-action@v5
  with:
    platforms: linux/amd64,linux/arm64
```

### Scheduled workflows
Run tests nightly to catch integration issues:

```yaml
on:
  schedule:
    - cron: '0 2 * * *'  # 2 AM UTC daily
```

### Dependabot for Actions
Keep actions up-to-date automatically:

Create `.github/dependabot.yml`:
```yaml
version: 2
updates:
  - package-ecosystem: "github-actions"
    directory: "/"
    schedule:
      interval: "weekly"
```

## Comparing GitHub Actions vs GitLab CI

| Feature | GitHub Actions | GitLab CI |
|---------|---------------|-----------|
| **Configuration** | `.github/workflows/*.yml` | `.gitlab-ci.yml` |
| **Secrets** | Built-in `GITHUB_TOKEN` | Requires `GITHUB_TOKEN` variable |
| **Container Registry** | ghcr.io (native) | ghcr.io (requires auth) |
| **Runner** | GitHub-hosted (free for public) | GitLab-hosted or self-hosted |
| **Caching** | GitHub Actions cache | GitLab cache or Docker layers |
| **Artifacts** | Built-in retention | Configurable expiration |
| **Matrix builds** | Native support | Manual configuration |

## Resources

- [GitHub Actions Documentation](https://docs.github.com/en/actions)
- [Docker/build-push-action](https://github.com/docker/build-push-action)
- [GitHub Container Registry](https://docs.github.com/en/packages/working-with-a-github-packages-registry/working-with-the-container-registry)
- [Semantic Versioning](https://semver.org/)
