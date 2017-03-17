'use strict';

require('babel-polyfill');
const gyp = require('gyp.js');

try {
  const code = gyp.main(bindings.argv.slice(1));
  if (code !== null) {
    bindings.error('Failed with code: ' + code);
    bindings.exit(-1);
  }
} catch (e) {
  bindings.error(e.stack);
  bindings.exit(-1);
}
bindings.exit(0);
