-- jsonb strings take their length from the copy in the database encoding.
-- U+0000 is rejected with the error of a json to jsonb cast.
CREATE FUNCTION se_jsonb_nul() RETURNS jsonb AS $$ return {a: 'x\u0000y'}; $$ LANGUAGE plv8;
SELECT se_jsonb_nul();
CREATE FUNCTION se_jsonb_from_json(j json) RETURNS jsonb AS $$ return j; $$ LANGUAGE plv8;
SELECT se_jsonb_from_json('{"a": "x\u0000y"}');
SELECT current_database() AS regression_db \gset
-- Stop here if the database cannot be created, it may belong to someone else.
\set ON_ERROR_STOP 1
CREATE DATABASE plv8_win1251 ENCODING 'WIN1251' LC_COLLATE 'C' LC_CTYPE 'C' TEMPLATE template0;
\c plv8_win1251
\set ON_ERROR_STOP 0
CREATE EXTENSION plv8;
CREATE FUNCTION se_cyrillic(n int) RETURNS jsonb AS $$
  return {a: String.fromCharCode(0x44f).repeat(n)};
$$ LANGUAGE plv8;
SELECT octet_length(se_cyrillic(1000)->>'a') AS bytes, length(se_cyrillic(1000)->>'a') AS chars;
-- jsonb strings reach JavaScript converted from the database encoding.
CREATE FUNCTION se_jsonb_codes(j jsonb) RETURNS text AS $$
  var k = Object.keys(j)[0];
  return k.length + ':' + k.charCodeAt(0) + ' ' + j[k].length + ':' + j[k].charCodeAt(0);
$$ LANGUAGE plv8;
CREATE FUNCTION se_jsonb_same(j jsonb) RETURNS jsonb AS $$ return j; $$ LANGUAGE plv8;
SELECT se_jsonb_codes(jsonb_build_object(chr(255), chr(255) || chr(255))) AS codes,
  se_jsonb_same(jsonb_build_object(chr(255), 'x')) = jsonb_build_object(chr(255), 'x') AS same;
\c :regression_db
DROP DATABASE plv8_win1251;

-- Script code that runs during a conversion of a result (a getter, valueOf(),
-- toString(), a Proxy trap) and throws: the error reaches SQL.
CREATE FUNCTION se_jsonb_getter() RETURNS jsonb AS $$
  return {get a() { throw new Error('getter'); }};
$$ LANGUAGE plv8;
SELECT se_jsonb_getter();
CREATE FUNCTION se_jsonb_proxy() RETURNS jsonb AS $$
  return new Proxy({}, {ownKeys() { throw new Error('ownKeys'); }});
$$ LANGUAGE plv8;
SELECT se_jsonb_proxy();
CREATE FUNCTION se_jsonb_symbol() RETURNS jsonb AS $$ return {a: Symbol('s')}; $$ LANGUAGE plv8;
SELECT se_jsonb_symbol();
CREATE FUNCTION se_jsonb_cycle() RETURNS jsonb AS $$
  var a = {};
  a.self = a;
  try {
    plv8.execute('SELECT $1::jsonb', [a]);
  } catch (e) {
    plv8.elog(NOTICE, 'cycle: ' + e.message);
  }
  return {};
$$ LANGUAGE plv8;
SELECT se_jsonb_cycle();
CREATE FUNCTION se_date() RETURNS timestamptz AS $$
  var d = new Date(0);
  d.valueOf = function() { throw new Error('valueOf'); };
  return d;
$$ LANGUAGE plv8;
SELECT se_date();
CREATE FUNCTION se_jsonb_date() RETURNS jsonb AS $$
  var d = new Date(0);
  d.valueOf = function() { throw new Error('valueOf'); };
  return {a: d};
$$ LANGUAGE plv8;
SELECT se_jsonb_date();
CREATE FUNCTION se_int_array() RETURNS int[] AS $$
  var a = [1, 2];
  Object.defineProperty(a, 1, {get() { throw new Error('getter'); }});
  return a;
$$ LANGUAGE plv8;
SELECT se_int_array();
CREATE TYPE se_rec AS (x int);
CREATE FUNCTION se_record() RETURNS se_rec AS $$
  return {get x() { throw new Error('getter'); }};
$$ LANGUAGE plv8;
SELECT * FROM se_record();
CREATE FUNCTION se_srf() RETURNS SETOF int AS $$
  var a = [1, 2];
  Object.defineProperty(a, 1, {get() { throw new Error('getter'); }});
  return a;
$$ LANGUAGE plv8;
SELECT * FROM se_srf();
CREATE FUNCTION se_srf_record() RETURNS SETOF se_rec AS $$
  return [new Proxy({x: 1}, {ownKeys() { throw new Error('ownKeys'); }})];
$$ LANGUAGE plv8;
SELECT * FROM se_srf_record();
CREATE TYPE se_rec2 AS (x int, y int);
CREATE FUNCTION se_record_names() RETURNS se_rec2 AS $$
  Object.defineProperty(Array.prototype, '1', {
    get() { return Symbol('n'); }, configurable: true });
  return {x: 1};
$$ LANGUAGE plv8;
SELECT * FROM se_record_names();
DO $$ delete Array.prototype[1]; $$ LANGUAGE plv8;
CREATE FUNCTION se_text() RETURNS text AS $$
  return {toString() { throw new Error('toString'); }};
$$ LANGUAGE plv8;
SELECT se_text();
CREATE FUNCTION se_text_symbol() RETURNS text AS $$ return Symbol('s'); $$ LANGUAGE plv8;
SELECT se_text_symbol();
CREATE FUNCTION se_json() RETURNS json AS $$ return {a: 1}; $$ LANGUAGE plv8;
DO $$
  globalThis.saved_JSON = JSON;
  Object.defineProperty(globalThis, 'JSON', {
    get() { throw new Error('JSON getter'); }, configurable: true });
$$ LANGUAGE plv8;
SELECT se_json();
SELECT plv8_info();
DO $$
  Object.defineProperty(globalThis, 'JSON', {
    value: globalThis.saved_JSON, writable: true, configurable: true });
  globalThis.saved_stringify = JSON.stringify;
  JSON.stringify = function() { return Symbol('s'); };
$$ LANGUAGE plv8;
SELECT se_json();
DO $$ JSON.stringify = globalThis.saved_stringify; $$ LANGUAGE plv8;
SELECT se_json();
-- plv8_info() lists the context of a role that has been dropped.  Stop if
-- the role cannot be created, it may belong to someone else.
\set ON_ERROR_STOP 1
CREATE ROLE se_gone_role;
\set ON_ERROR_STOP 0
GRANT EXECUTE ON FUNCTION se_json() TO se_gone_role;
SET ROLE se_gone_role;
SELECT se_json();
RESET ROLE;
REVOKE EXECUTE ON FUNCTION se_json() FROM se_gone_role;
DROP ROLE se_gone_role;
SELECT plv8_info() IS NOT NULL AS info;
CREATE FUNCTION se_prepare_late() RETURNS jsonb AS $$
  return {get a() { plv8.prepare('SELECT $1::int AS x'); return 1; }};
$$ LANGUAGE plv8;
SELECT se_prepare_late();

-- The same inside calls from JavaScript: the script can catch the error.
DO $$
  var getter = {get() { throw new Error('getter'); }};
  var valueOf = {valueOf() { throw new Error('valueOf'); }};
  function params() { var a = [1]; Object.defineProperty(a, 0, getter); return a; }
  var plan = plv8.prepare('SELECT $1::int AS x', ['int']);
  var cursor_plan = plv8.prepare('SELECT 1 AS x');
  var cursor = cursor_plan.cursor();
  var calls = {
    'execute': function() { plv8.execute('SELECT $1::int', params()); },
    'execute symbol': function() { plv8.execute(Symbol('q')); },
    'execute jsonb': function() { plv8.execute('SELECT $1::jsonb', [params()]); },
    'plan.execute': function() { plan.execute(params()); },
    'plan.cursor': function() { plan.cursor(params()); },
    'prepare': function() {
      var t = ['int'];
      Object.defineProperty(t, 0, getter);
      plv8.prepare('SELECT $1', t);
    },
    'prepare symbol': function() { plv8.prepare('SELECT $1', [Symbol('t')]); },
    'elog': function() { plv8.elog(valueOf, 'x'); },
    'fetch': function() { cursor.fetch(valueOf); },
    'move': function() { cursor.move(valueOf); },
    'find_function': function() { plv8.find_function(Symbol('f')); },
    'quote_literal': function() { plv8.quote_literal(Symbol('q')); },
    'quote_nullable': function() { plv8.quote_nullable(Symbol('q')); },
    'quote_ident': function() { plv8.quote_ident(Symbol('q')); }
  };
  for (var name in calls) {
    try {
      calls[name]();
      plv8.elog(NOTICE, name + ': no error');
    } catch (e) {
      plv8.elog(NOTICE, name + ': ' + e.message);
    }
  }
  cursor.close();
  cursor_plan.free();
  plan.free();
$$ LANGUAGE plv8;
CREATE FUNCTION se_window() RETURNS int AS $$
  var w = plv8.get_window_object();
  var v = {valueOf() { throw new Error('valueOf'); }};
  var calls = {
    'get_func_arg_in_partition': function() { w.get_func_arg_in_partition(v, 0, w.SEEK_HEAD, false); },
    'get_func_arg_in_frame': function() { w.get_func_arg_in_frame(0, v, w.SEEK_HEAD, false); },
    'get_func_arg_current': function() { w.get_func_arg_current(v); },
    'set_mark_position': function() { w.set_mark_position(v); },
    'rows_are_peers': function() { w.rows_are_peers(v, 0); },
    'get_partition_local': function() { w.get_partition_local(v); }
  };
  for (var name in calls) {
    try {
      calls[name]();
      plv8.elog(NOTICE, name + ': no error');
    } catch (e) {
      plv8.elog(NOTICE, name + ': ' + e.message);
    }
  }
  return 1;
$$ LANGUAGE plv8 WINDOW;
SELECT se_window() OVER () FROM generate_series(1, 1);

-- Objects that plv8 builds get own properties: setters on the prototypes do
-- not run, and a "__proto__" key stays a key.
DO $$
  Object.defineProperty(Object.prototype, 'x', {
    set(v) { throw new Error('setter'); }, configurable: true });
  Object.defineProperty(Array.prototype, '0', {
    set(v) { throw new Error('setter'); }, configurable: true });
  try {
    plv8.elog(NOTICE, JSON.stringify(plv8.execute('SELECT 1 AS x')));
  } finally {
    delete Object.prototype.x;
    delete Array.prototype[0];
  }
$$ LANGUAGE plv8;
CREATE FUNCTION se_jsonb_identity(j jsonb) RETURNS jsonb AS $$ return j; $$ LANGUAGE plv8;
SELECT se_jsonb_identity('{"__proto__": {"x": 1}, "a": 2}');
DO $$
  var row = plv8.execute('SELECT 1 AS __proto__, 2 AS a')[0];
  plv8.elog(NOTICE, JSON.stringify(Object.keys(row)));
$$ LANGUAGE plv8;

-- A plan freed by script code that runs while its parameters are converted.
DO $$
  function freeing(plan) {
    var a = [1, 2];
    Object.defineProperty(a, 0, {get() { plan.free(); return 1; }});
    return a;
  }
  var p1 = plv8.prepare('SELECT $1::int + $2::int AS x', ['int', 'int']);
  try { p1.execute(freeing(p1)); } catch (e) { plv8.elog(NOTICE, 'execute: ' + e.message); }
  var p2 = plv8.prepare('SELECT $1::int + $2::int AS x');
  try { p2.execute(freeing(p2)); } catch (e) { plv8.elog(NOTICE, 'variable parameters: ' + e.message); }
  var p3 = plv8.prepare('SELECT $1::int + $2::int AS x', ['int', 'int']);
  try { p3.cursor(freeing(p3)); } catch (e) { plv8.elog(NOTICE, 'cursor: ' + e.message); }
$$ LANGUAGE plv8;

-- Script code that runs SQL or closes the cursor in the middle of a call.
DO $$
  var plan = plv8.prepare('SELECT generate_series(1, 3) AS x');
  var cursor = plan.cursor();
  var n = {valueOf() { cursor.close(); return 1; }};
  try { cursor.fetch(n); } catch (e) { plv8.elog(NOTICE, 'fetch: ' + e.message); }
  plan.free();
  var parse = JSON.parse;
  JSON.parse = function(s) {
    plv8.execute('SELECT 42 AS nested');
    plv8.execute('SET work_mem = 4096');
    return parse(s);
  };
  try {
    var rows = plv8.execute(
      "SELECT x, ('{\"v\":' || x || '}')::json AS j FROM generate_series(1, 3) x");
    plv8.elog(NOTICE, JSON.stringify(rows));
  } finally {
    JSON.parse = parse;
  }
  var skipped = plv8.prepare('SELECT $2::int AS x');
  try { skipped.execute([1, 2]); } catch (e) { plv8.elog(NOTICE, 'skipped parameter: ' + e.message); }
  skipped.free();
  try { throw new Error('next'); } catch (e) { plv8.elog(NOTICE, 'then: ' + e.message); }
$$ LANGUAGE plv8;
RESET work_mem;

-- A plan with variable parameters prepared during a jsonb conversion keeps
-- its parameter state after the conversion.
DO $$
  var holder = {};
  plv8.execute('SELECT $1::jsonb', [{get a() {
    holder.plan = plv8.prepare('SELECT $1::int + 1 AS x');
    return 1;
  }}]);
  for (var i = 0; i < 20; i++)
    plv8.execute('SELECT $1::jsonb', [{k: 'v'.repeat(200), n: [1, 2, 3, 4, 5, 6, 7, 8]}]);
  plv8.elog(NOTICE, JSON.stringify(holder.plan.execute([41])));
  holder.plan.free();
$$ LANGUAGE plv8;

-- An error in a cursor operation rolls back a subtransaction only.
DO $$
  var plan = plv8.prepare('SELECT 1 / (x - 3) AS y FROM generate_series(1, 5) x');
  var cursor = plan.cursor();
  try { cursor.fetch(10); } catch (e) { plv8.elog(NOTICE, 'fetch: ' + e.message); }
  try { cursor.fetch(1); } catch (e) {
    plv8.elog(NOTICE, 'fetch again: ' + e.message.replace(/portal \d+/, 'portal N'));
  }
  cursor.close();
  plan.free();
  plv8.elog(NOTICE, JSON.stringify(plv8.execute('SELECT 2 AS z')));
$$ LANGUAGE plv8;
