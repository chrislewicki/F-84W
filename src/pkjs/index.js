var Clay = require('pebble-clay');
var clayConfig = require('./config');

// Clay automatically handles 'showConfiguration' and 'webviewclosed' and sends
// the chosen settings to the watch via AppMessage using the messageKeys above.
var clay = new Clay(clayConfig);
