// The driver of the web integration test: telosnex_web_test's, which saves
// the page console to web_console.log while the test runs, always writes the
// report data, and prints each failure.
import 'package:telosnex_web_test/driver.dart';

Future<void> main() => webIntegrationDriver();
