module.exports = [
  {
    type: "heading",
    defaultValue: "F-84W"
  },
  {
    type: "text",
    defaultValue: "Settings for F-84W"
  },
  {
    type: "section",
    items: [
      { type: "heading", defaultValue: "Display" },
      {
        type: "toggle",
        messageKey: "SHOW_SECONDS",
        label: "Show seconds",
        defaultValue: true
      }
    ]
  },
  {
    type: "section",
    items: [
      { type: "heading", defaultValue: "Sound" },
      {
        type: "slider",
        messageKey: "CHIME_VOLUME",
        label: "Chime volume",
        defaultValue: 50,
        min: 0,
        max: 100,
        step: 10,
        description: "Volume of the hourly chime and alarm beep. Speaker models only (Pebble 2 Duo, Pebble Time 2)."
      }
    ]
  },
  {
    type: "section",
    items: [
      { type: "heading", defaultValue: "Hourly signal" },
      {
        type: "toggle",
        messageKey: "HOURLY_VIBE",
        label: "Vibrate on the hour",
        defaultValue: false
      },
      {
        type: "toggle",
        messageKey: "HOURLY_CHIME",
        label: "Chime on the hour",
        defaultValue: false,
        description: "Plays the classic Casio \"beep-beep,\" at least as well as the speaker can."
      }
    ]
  },
  {
    type: "section",
    items: [
      { type: "heading", defaultValue: "Alarm" },
      {
        type: "toggle",
        messageKey: "ALARM_ENABLED",
        label: "Enable alarm",
        defaultValue: false
      },
      {
        type: "input",
        messageKey: "ALARM_TIME",
        label: "Alarm time",
        defaultValue: "07:00",
        attributes: { type: "time" }
      },
      {
        type: "toggle",
        messageKey: "ALARM_VIBE",
        label: "Vibrate",
        defaultValue: true
      },
      {
        type: "toggle",
        messageKey: "ALARM_CHIME",
        label: "Chime",
        defaultValue: true,
        description: "Plays the classic Casio \"beep-beep\" but this time repeatedly, just like the actual watch."
      },
      {
        type: "text",
        defaultValue: "Once the alarm starts, shake your wrist to silence it."
      }
    ]
  },
  {
    type: "submit",
    defaultValue: "Save"
  }
];
