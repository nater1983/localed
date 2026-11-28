# Changelog

## 0.1.0 (unreleased)

- Extract GNOME, Plasma 6, AccountsService and shared session monitors from localed.
- Use a separate executable and helper; do not claim the locale1 bus name.
- Observe by default; require an explicit administrator switch for automatic synchronization.
- Route enabled writes through locale1 instead of in-process privileged setters.
- Carry over backend lifecycle tests and add an observation-mode test.
