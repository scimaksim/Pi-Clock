# Home Assistant alerts

Pi-Clock can post sky alerts to a Home Assistant **webhook** so an
automation can notify your phone, flash a light or speak on a speaker.

## 1. Create the automation

Settings → Automations & Scenes → Create automation → ⋮ → Edit in YAML,
paste this, change the webhook ID and the `notify.mobile_app_...` line
to your phone, then save.

```yaml
alias: Pi-Clock sky alerts
description: Passes and aurora alerts from Pi-Clock
mode: queued
triggers:
  - trigger: webhook
    webhook_id: pi-clock-CHANGE-ME        # keep this secret
    allowed_methods: [POST]
    local_only: true
actions:
  - action: notify.mobile_app_YOUR_PHONE   # Developer tools → Actions → type "notify."
    data:
      title: "{{ trigger.json.title }}"
      message: "{{ trigger.json.message }}"
      data:
        tag: "pi-clock-{{ trigger.json.event }}"
        group: pi-clock
  # Optional extras for aurora — uncomment and pick your entities:
  # - if: "{{ trigger.json.event == 'aurora_now' }}"
  #   then:
  #     - action: light.turn_on
  #       target: { entity_id: light.living_room }
  #       data: { flash: long, color_name: green }
  #     - action: tts.speak
  #       target: { entity_id: tts.google_translate_en_com }
  #       data:
  #         media_player_entity_id: media_player.kitchen
  #         message: "{{ trigger.json.message }}"
```

## 2. Point Pi-Clock at it

Dashboard → **Home Assistant Alerts** → paste
`http://<home-assistant-ip>:8123/api/webhook/<your id>` → Save URL →
**Send Test**. Use the IP address; the Pi doesn't resolve `.local`
names.

## Events

Every payload has `source: "pi-clock"`, `event`, `title`, `message`.

| event | when | extra fields |
|---|---|---|
| `pass` | a visible pass starts in *lead* minutes and peaks above the minimum elevation | `sat`, `start` (UTC ISO), `minutes`, `duration_min`, `max_el`, `from`, `to` |
| `aurora_now` | dark at home and home is north of the photo (or view) line; repeats every 3 h while active | `level` (`photo`/`visible`), `kp`, `clouds` (% or -1) |
| `aurora_forecast` | the Kp forecast reaches home's level within 24 h (once per forecast block) | `level`, `kp`, `when` (UTC ISO), `clouds` |
| `test` | the dashboard's Send Test button | — |

`clouds` comes from the Astrospheric forecast when the Tonight's Sky
panel is enabled, so an automation can skip cloudy alerts with a
condition like `{{ trigger.json.clouds < 50 }}`.
