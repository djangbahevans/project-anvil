const App = ({ name }) => (
  <div className="app" onClick={() => alert('hi')}>
    <Greeting name={name} />
    {items.map(i => <li key={i}>{i}</li>)}
  </div>
);
