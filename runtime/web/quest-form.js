// The custom quest creator's form (the launcher's QuestForm, launcher.cpp):
// every option of a quest from scratch, for the mouse and the keyboard. The
// launcher opens it with the quest and the choices, polls it while it waits,
// and reads the settings back as "key=value" lines; a quest the game cannot
// take comes back with the reason and the form stays open.
'use strict';

window.mh3QuestForm = (() => {
  const dialog = document.getElementById('quest-form');
  const form = dialog.querySelector('form');
  const field = (id) => document.getElementById(`qf-${id}`);
  const monsterSelects = [0, 1, 2].map((i) => field(`monster${i}`));
  const sizeInputs = [0, 1, 2].map((i) => field(`size${i}`));
  const arrivals = [...form.querySelectorAll('input[name=arrival]')];
  const ranks = ['Village', 'Low rank', 'High rank'];
  const whens = { 1: 'after 3 minutes', 2: 'with one monster out', 3: 'with two monsters out' };

  // closed, open, saved (the lines wait for the launcher) or checking.
  let state = 'closed';
  let result = '';
  let choices = { maps: [], monsters: [] };

  const monster = (id) => choices.monsters.find((m) => m.id === id);
  const map = () => choices.maps.find((m) => m.id === Number(field('map').value));
  const chosen = () => monsterSelects.map((select) => Number(select.value));
  const count = () => chosen().filter(Boolean).length;
  const arrival = () => Number(arrivals.find((radio) => radio.checked).value);

  function option(select, value, text) {
    const element = document.createElement('option');
    element.value = value;
    element.textContent = text;
    select.append(element);
  }

  // The goal as the launcher names a quest whose name nobody typed.
  function goal() {
    const ids = chosen().filter(Boolean);
    const names = ids.map((id, i) => {
      const size = Number(sizeInputs[i].value);
      return (size <= 80 ? 'Tiny ' : size >= 120 ? 'Giant ' : '') + (monster(id)?.name ?? `monster ${id}`);
    });

    const list = names.length < 2 ? names.join('') : `${names.slice(0, -1).join(', ')} and ${names[names.length - 1]}`;
    return `${field('capture').checked ? 'Capture' : 'Hunt'} ${list}`.slice(0, 43);
  }

  // The monsters the map takes; a second and a third may be none, and come
  // only beside monsters that are not alone (Ceadeus, Alatreon, Jhen Mohran).
  function fillMonsters() {
    const allowed = map().monsters;
    const previous = chosen();

    monsterSelects.forEach((select, i) => {
      select.replaceChildren();

      if (i)
        option(select, 0, 'None');

      for (const id of allowed)
        if (!i || !monster(id).alone)
          option(select, id, monster(id).name);

      select.value = allowed.includes(previous[i]) && (!i || !monster(previous[i]).alone) ? previous[i] : i ? 0 : allowed[0];
    });
  }

  function fillIntruders(current) {
    const select = field('intruder');
    select.replaceChildren();
    option(select, '', 'None');

    map().intruders.forEach((intruder, i) => option(select, i, `Group ${intruder.group} (as in "${intruder.quest}")`));
    const index = current ? map().intruders.findIndex((i) => i.table === current.table && i.group === current.group) : -1;
    select.value = current && current.chance && index >= 0 ? index : '';

    if (select.value !== '') {
      field('chance').value = current.chance;
      field('when').value = current.when;
    }
  }

  // Everything that depends on other fields: which monsters, arrivals,
  // captures and intruders can be chosen, and the notes saying why not.
  function update() {
    const first = monster(chosen()[0]);

    if (first.alone) {
      monsterSelects[1].value = 0;
      monsterSelects[2].value = 0;
    }

    monsterSelects[1].disabled = first.alone || arrival() === 1;
    monsterSelects[2].disabled = first.alone || !chosen()[1];

    if (!chosen()[1])
      monsterSelects[2].value = 0;

    // The monster that waits comes with the first one's files: the same.
    if (arrival() === 1) {
      monsterSelects[1].value = chosen()[0];

      if (chosen()[2])
        monsterSelects[2].value = chosen()[0];
    }

    const n = count();
    const allowed = { 0: true, 1: n >= 2 && !first.alone, 2: n === 3, 3: n >= 2 };

    for (const radio of arrivals) {
      radio.disabled = !allowed[radio.value];

      if (radio.checked && radio.disabled)
        arrivals[0].checked = true;
    }

    sizeInputs.forEach((input, i) => {
      input.disabled = !chosen()[i];
    });

    const capturable = map().capture && chosen().filter(Boolean).every((id) => monster(id).capture);
    field('capture').disabled = !capturable;

    if (!capturable)
      field('capture').checked = false;

    field('hunt').checked = !field('capture').checked;
    field('capture-note').textContent = capturable ? '' : map().capture ? 'Not every monster here can be captured.' : 'Captures are for the five field maps.';

    // The intruder shares the extra monster's state with the other arrivals.
    const intruders = arrival() === 0 && n <= 2 && map().intruders.length > 0;
    field('intruder').disabled = !intruders;

    if (!intruders)
      field('intruder').value = '';

    const on = field('intruder').value !== '';
    field('chance').disabled = !on;
    field('when').disabled = !on;
    field('intruder-note').textContent = !map().intruders.length ? 'No quest on this map has an intruder.'
      : arrival() !== 0 ? 'Intruders come only when the monsters arrive together.'
      : n > 2 ? 'Intruders come beside one or two monsters.' : '';

    const lines = [`${ranks[Number(field('rank').value)]}, ${field('stars').value}★, ${field('minutes').value} minutes.`];

    if (arrival() === 3)
      lines.push('One after another leaves out the break and capture rewards.');

    if (arrival() === 1)
      lines.push('The quest ends when every one of them falls.');

    field('summary').textContent = lines.join(' ');
    field('name').placeholder = goal();
  }

  function setError(text) {
    const error = field('error');
    error.textContent = text;
    error.hidden = !text;
  }

  function open(json) {
    const data = JSON.parse(json);
    const quest = data.quest;
    choices = data;
    field('title').textContent = data.title;
    field('map').replaceChildren();

    for (const m of data.maps)
      option(field('map'), m.id, m.name);

    field('map').value = quest.map;
    fillMonsters();
    quest.monsters.forEach((id, i) => {
      monsterSelects[i].value = id;
    });
    quest.sizes.forEach((size, i) => {
      sizeInputs[i].value = size;
    });
    field('rank').value = quest.rank;
    field('stars').value = quest.stars;
    field('minutes').value = quest.minutes || 50;
    field('capture').checked = quest.capture;
    arrivals.forEach((radio) => {
      radio.checked = Number(radio.value) === quest.arrival;
    });
    fillIntruders(quest.intruder);
    field('patch').value = quest.patch;
    field('patch-details').open = quest.patch !== '';
    update();

    // A name that follows the goal stays empty: the placeholder shows it.
    field('name').value = quest.name === goal() ? '' : quest.name;
    setError('');
    state = 'open';
    result = '';

    // Filling the window, SDL moves the page's other elements into a hidden
    // block (SDL_SetWindowFillDocument): the form goes beside the canvas.
    if (dialog.parentNode !== document.body)
      document.body.append(dialog);

    if (!dialog.open)
      dialog.showModal();

    field('name').focus();
  }

  function lines() {
    const intruder = field('intruder').value === '' ? null : map().intruders[Number(field('intruder').value)];
    const values = {
      name: field('name').value.trim(),
      map: field('map').value,
      monsters: chosen().join(','),
      sizes: sizeInputs.map((input) => Math.round(Number(input.value)) || 100).join(','),
      rank: field('rank').value,
      stars: field('stars').value,
      minutes: field('minutes').value,
      capture: field('capture').checked ? 1 : 0,
      arrival: arrival(),
      intruder: intruder ? [field('chance').value, intruder.table, field('when').value, intruder.group].join(',') : '0,0,0,0',
      patch: field('patch').value.trim(),
    };

    return Object.entries(values).map(([key, value]) => `${key}=${value}`).join('\n');
  }

  form.addEventListener('submit', (event) => {
    event.preventDefault();

    if (state !== 'open' || !form.reportValidity())
      return;

    result = lines();
    state = 'saved';
    setError('');
    field('save').disabled = true;
  });

  field('cancel').addEventListener('click', () => dialog.close());

  // Not while the launcher checks a quest: its answer comes first.
  dialog.addEventListener('cancel', (event) => {
    if (state !== 'open')
      event.preventDefault();
  });

  dialog.addEventListener('close', () => {
    if (state === 'open')
      state = 'closed';
  });

  // The keys stay in the form: the game's own handlers (SDL's, on the window)
  // would take them and stop the typing. Releases go on: a key that opened the
  // form (Enter on the launcher's row) would otherwise stay held for the game.
  for (const type of ['keydown', 'keypress'])
    dialog.addEventListener(type, (event) => event.stopPropagation());

  field('map').addEventListener('change', () => {
    fillMonsters();
    fillIntruders(null);
    update();
  });

  field('intruder').addEventListener('change', () => {
    const intruder = map().intruders[Number(field('intruder').value)];

    if (intruder) {
      field('chance').value = intruder.chance;
      field('when').value = intruder.when;
    }

    update();
  });

  form.addEventListener('input', update);
  form.addEventListener('change', update);

  return {
    open,

    // -1 while open or checking, 0 closed, else the lines' length in bytes.
    poll() {
      if (state === 'closed')
        return 0;

      if (state !== 'saved')
        return -1;

      state = 'checking';
      return new TextEncoder().encode(result).length;
    },

    result: () => result,

    fail(message) {
      state = 'open';
      field('save').disabled = false;
      setError(message);

      if (!dialog.open)
        dialog.showModal();
    },

    close() {
      state = 'closed';
      field('save').disabled = false;
      dialog.close();
    },
  };
})();
