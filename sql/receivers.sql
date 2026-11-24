-- Scripts can reassign the global plv8.  Window and set-returning calls then
-- report an error instead of using another object as the plv8 object.
CREATE FUNCTION plv8_hide(replacement text) RETURNS void AS $$
  globalThis.saved_plv8 = plv8;
  globalThis.plv8 = replacement === 'object' ? {} : 1;
$$ LANGUAGE plv8;
CREATE FUNCTION plv8_restore() RETURNS void AS $$
  globalThis.plv8 = globalThis.saved_plv8;
$$ LANGUAGE plv8;
CREATE FUNCTION recv_win() RETURNS int AS $$ return 1; $$ LANGUAGE plv8 WINDOW;
CREATE FUNCTION recv_srf() RETURNS SETOF int AS $$ return [1, 2]; $$ LANGUAGE plv8;
SELECT plv8_hide('object');
SELECT recv_win() OVER () FROM generate_series(1, 1);
SELECT * FROM recv_srf();
SELECT plv8_restore();
SELECT plv8_hide('number');
SELECT recv_win() OVER () FROM generate_series(1, 1);
SELECT * FROM recv_srf();
SELECT plv8_restore();
SELECT recv_win() OVER () FROM generate_series(1, 1);
SELECT * FROM recv_srf();

-- A getter on the global plv8 that throws is reported the same way.
CREATE FUNCTION plv8_throwing_getter() RETURNS void AS $$
  globalThis.saved_plv8 = plv8;
  Object.defineProperty(globalThis, 'plv8', {
    get() { throw new Error('getter failed'); }, configurable: true });
$$ LANGUAGE plv8;
CREATE FUNCTION plv8_restore_value() RETURNS void AS $$
  Object.defineProperty(globalThis, 'plv8', {
    value: globalThis.saved_plv8, writable: true, configurable: true });
$$ LANGUAGE plv8;
SELECT plv8_throwing_getter();
SELECT recv_win() OVER () FROM generate_series(1, 1);
SELECT * FROM recv_srf();
SELECT plv8_restore_value();
SELECT recv_win() OVER () FROM generate_series(1, 1);

-- return_next accepts only the plv8 object of a set-returning call.
CREATE FUNCTION recv_return_next() RETURNS void AS $$
  plv8.return_next.call({}, 1);
$$ LANGUAGE plv8;
SELECT recv_return_next();

-- After a set-returning call fails on its result, return_next has no set to
-- write to.
CREATE FUNCTION recv_srf_big() RETURNS SETOF numeric AS $$
  return [1, 10n ** 140000n];
$$ LANGUAGE plv8;
SELECT * FROM recv_srf_big();
DO $$ plv8.return_next(1); $$ LANGUAGE plv8;

-- Conversion errors inside a call reach the script as exceptions.
CREATE FUNCTION recv_return_next_big() RETURNS SETOF numeric AS $$
  plv8.return_next(1);
  try {
    plv8.return_next(10n ** 140000n);
  } catch (e) {
    plv8.return_next(2);
  }
$$ LANGUAGE plv8;
SELECT * FROM recv_return_next_big();
DO $$
  try {
    plv8.execute('SELECT 10::numeric ^ 400 AS x');
  } catch (e) {
    plv8.elog(NOTICE, 'out of range caught: ' + (e.message.indexOf('out of range') >= 0));
  }
$$ LANGUAGE plv8;
DO $$
  try {
    plv8.prepare('SELECT $1', ['no_such_type']);
  } catch (e) {
    plv8.elog(NOTICE, 'caught: ' + e.message);
  }
$$ LANGUAGE plv8;
DO $$
  var plan = plv8.prepare('SELECT $1::int AS x', ['int']);
  try {
    plan.execute(['abc']);
  } catch (e) {
    plv8.elog(NOTICE, 'caught: ' + e.message);
  }
  try {
    plan.cursor(['abc']);
  } catch (e) {
    plv8.elog(NOTICE, 'cursor caught: ' + e.message);
  }
  plan.free();
$$ LANGUAGE plv8;
DO $$
  try {
    plv8.execute('SELECT ROW(10::numeric ^ 400) AS r');
  } catch (e) {
    plv8.elog(NOTICE, 'record caught: ' + (e.message.indexOf('out of range') >= 0));
  }
$$ LANGUAGE plv8;
