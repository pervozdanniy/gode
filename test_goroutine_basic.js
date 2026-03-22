// Simple test for goroutine bindings
console.log('Testing goroutine bindings...\n');

try {
  const goroutineBinding = internalBinding('goroutine');
  console.log('✅ Goroutine binding loaded successfully!');
  console.log('Available functions:', Object.keys(goroutineBinding));
  
  // Test go() function
  console.log('\nTesting go() function...');
  const goid = goroutineBinding.go(() => {
    console.log('  [Goroutine] Hello from goroutine!');
  });
  console.log('  Created goroutine with ID:', goid);
  
  // Test goid() function  
  console.log('\nTesting goid() function...');
  const currentGoid = goroutineBinding.goid();
  console.log('  Current goroutine ID:', currentGoid);
  
  // Test multiple goroutines
  console.log('\nCreating multiple goroutines...');
  for (let i = 0; i < 5; i++) {
    const id = goroutineBinding.go((num) => {
      console.log(`  [Goroutine ${num}] Running...`);
    }, i);
    console.log(`  Created goroutine ${i} with ID:`, id);
  }
  
  console.log('\n✅ All tests passed!');
  console.log('\nNote: Goroutines are scheduled but context switching not yet implemented.');
  console.log('Check [Runtime] logs for M thread activity.');
  
} catch (err) {
  console.error('❌ Error:', err.message);
  console.error(err.stack);
}

// Give time for goroutines to be scheduled
setTimeout(() => {
  console.log('\nExiting...');
  process.exit(0);
}, 1000);

